#include <Arduino.h>
#include <DictZip.h>
#include <Dictionary.h>
#include <DictionaryRegistry.h>
#include <HalStorage.h>
#include <Memory.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char* kFolder = "host-dictionary";
constexpr const char* kBase = "/dictionaries/host-dictionary/words";
constexpr size_t kSidecarHeaderBytes = 6 * sizeof(uint32_t);

struct Entry {
  std::string headword;
  std::string definition;
};

struct Alias {
  std::string spelling;
  uint32_t ordinal = 0;
};

struct EncodedDictionary {
  std::vector<uint8_t> idx;
  std::vector<uint8_t> dict;
  std::vector<uint8_t> syn;
};

void appendU32BE(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

uint32_t readU32Native(const std::vector<uint8_t>& bytes, size_t offset) {
  uint32_t value = 0;
  if (offset + sizeof(value) <= bytes.size()) std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

EncodedDictionary encode(const std::vector<Entry>& entries, std::vector<Alias> aliases = {}) {
  EncodedDictionary result;
  std::vector<uint32_t> offsets;
  offsets.reserve(entries.size());
  for (const auto& entry : entries) {
    offsets.push_back(static_cast<uint32_t>(result.dict.size()));
    result.dict.insert(result.dict.end(), entry.definition.begin(), entry.definition.end());
  }

  for (size_t i = 0; i < entries.size(); ++i) {
    result.idx.insert(result.idx.end(), entries[i].headword.begin(), entries[i].headword.end());
    result.idx.push_back(0);
    appendU32BE(result.idx, offsets[i]);
    appendU32BE(result.idx, static_cast<uint32_t>(entries[i].definition.size()));
  }

  std::sort(aliases.begin(), aliases.end(),
            [](const Alias& lhs, const Alias& rhs) { return lhs.spelling < rhs.spelling; });
  for (const auto& alias : aliases) {
    result.syn.insert(result.syn.end(), alias.spelling.begin(), alias.spelling.end());
    result.syn.push_back(0);
    appendU32BE(result.syn, alias.ordinal);
  }
  return result;
}

struct LegacyDictionaryLayout {
  std::string basePath;
  bool hasPlainDict = false;
  char wordBuf[256] = {};
};

}  // namespace

// Dictionary.cpp includes the production declarations, but host tests provide
// only these two narrow seams instead of linking the registry/SD dictzip stack.
namespace DictionaryRegistry {
namespace {
std::string basePath = kBase;
}

bool resolveBasePath(const char* folderName, std::string& basePathOut) {
  if (!folderName || std::string(folderName) != kFolder) return false;
  basePathOut = basePath;
  return true;
}
}  // namespace DictionaryRegistry

namespace DictZip {

bool extractEntry(const char* /*path*/, uint32_t /*offset*/, uint32_t /*size*/, HalFile& /*outFile*/,
                  ExtractError* outError) {
  if (outError) *outError = ExtractError::ReadError;
  return false;
}

}  // namespace DictZip

class DictionaryHostTest : public ::testing::Test {
 protected:
  void SetUp() override {
    HalStorage::resetTestFiles();
    HostMemory::reset();
    ESP.maxAllocHeap = std::numeric_limits<uint32_t>::max();
    ESP.freeHeap = std::numeric_limits<uint32_t>::max();
  }

  void TearDown() override {
    HalStorage::resetTestFiles();
    HostMemory::reset();
  }

  void install(const std::vector<Entry>& entries) { install(encode(entries), false); }

  void install(const std::vector<Entry>& entries, const std::vector<Alias>& aliases) {
    install(encode(entries, aliases), true);
  }

  void install(const EncodedDictionary& files, bool withSyn) {
    HalStorage::registerTestFile(std::string(kBase) + ".idx", files.idx);
    HalStorage::registerTestFile(std::string(kBase) + ".dict", files.dict);
    if (withSyn) HalStorage::registerTestFile(std::string(kBase) + ".syn", files.syn);
  }

  Dictionary openDictionary() {
    Dictionary dictionary;
    EXPECT_TRUE(dictionary.open(kFolder));
    return dictionary;
  }

  void buildIndex(Dictionary& dictionary) {
    Dictionary::IndexResult result = Dictionary::IndexResult::ReadError;
    ASSERT_TRUE(dictionary.buildIndex(nullptr, nullptr, &result));
    EXPECT_EQ(result, Dictionary::IndexResult::Ok);
  }

  static std::vector<Entry> numberedEntries(size_t count) {
    std::vector<Entry> entries;
    entries.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      char word[32];
      snprintf(word, sizeof(word), "word%04zu", i);
      entries.push_back({word, std::string("definition-") + word});
    }
    return entries;
  }

  static const std::string path(const char* suffix) { return std::string(kBase) + suffix; }
};

TEST_F(DictionaryHostTest, DirectHitKeepsSynonymFilesLazy) {
  install({{"cat", "direct definition"}}, {{"feline", 0}});
  Dictionary dictionary = openDictionary();
  buildIndex(dictionary);

  const size_t synOpensBefore = HalStorage::readOpenAttempts(path(".syn"));
  const size_t sidxOpensBefore = HalStorage::readOpenAttempts(path(".sidx"));
  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;

  EXPECT_TRUE(dictionary.lookup("cat", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::Found);
  EXPECT_EQ(headword, "cat");
  EXPECT_EQ(definition, "direct definition");
  EXPECT_EQ(HalStorage::readOpenAttempts(path(".syn")), synOpensBefore);
  EXPECT_EQ(HalStorage::readOpenAttempts(path(".sidx")), sidxOpensBefore);
}

TEST_F(DictionaryHostTest, SynonymResolvesOrdinalPastTheTwoHundredFiftySixthEntry) {
  const auto entries = numberedEntries(513);
  std::vector<Alias> aliases;
  for (size_t i = 0; i < 513; ++i) {
    char alias[32];
    snprintf(alias, sizeof(alias), "alias%04zu", i);
    aliases.push_back({alias, static_cast<uint32_t>(i)});
  }
  install(entries, aliases);
  Dictionary dictionary = openDictionary();
  buildIndex(dictionary);

  const auto qidx = HalStorage::readTestFile(path(".qidx"));
  const auto sidx = HalStorage::readTestFile(path(".sidx"));
  ASSERT_GE(qidx.size(), kSidecarHeaderBytes);
  ASSERT_GE(sidx.size(), kSidecarHeaderBytes);
  EXPECT_EQ(readU32Native(qidx, 4), 2u);
  EXPECT_EQ(readU32Native(sidx, 4), 2u);
  EXPECT_EQ(readU32Native(qidx, 12), 3u);  // entry 0, 256, and 512 samples
  EXPECT_EQ(readU32Native(qidx, 20), 513u);
  EXPECT_EQ(readU32Native(sidx, 12), 3u);

  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  EXPECT_TRUE(dictionary.lookup("alias0511", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::Found);
  EXPECT_EQ(headword, entries[511].headword);
  EXPECT_EQ(definition, entries[511].definition);

  definition.clear();
  headword.clear();
  EXPECT_TRUE(dictionary.lookup("alias0512", definition, headword, &result));
  EXPECT_EQ(headword, entries[512].headword);
  EXPECT_EQ(definition, entries[512].definition);
}

TEST_F(DictionaryHostTest, ExactIndexPrecedesSynonymAndSynonymPrecedesStemming) {
  const std::vector<Entry> entries = {
      {"color", "color definition"},
      {"colour", "direct colour definition"},
      {"play", "stem definition"},
      {"special", "synonym definition"},
  };
  const std::vector<Alias> aliases = {{"colour", 0}, {"playing", 3}};
  install(entries, aliases);
  Dictionary dictionary = openDictionary();
  buildIndex(dictionary);

  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;

  EXPECT_TRUE(dictionary.lookup("colour", definition, headword, &result));
  EXPECT_EQ(headword, "colour");
  EXPECT_EQ(definition, "direct colour definition");

  definition.clear();
  headword.clear();
  EXPECT_TRUE(dictionary.lookup("playing", definition, headword, &result));
  EXPECT_EQ(headword, "special");
  EXPECT_EQ(definition, "synonym definition");
}

TEST_F(DictionaryHostTest, MissingSynonymFileLeavesOrdinaryMissesUnchanged) {
  install({{"dog", "dog definition"}});
  Dictionary dictionary = openDictionary();
  buildIndex(dictionary);

  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::Found;
  EXPECT_FALSE(dictionary.lookup("unlisted", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::NotFound);
  EXPECT_EQ(HalStorage::readOpenAttempts(path(".syn")), 0u);
  EXPECT_EQ(HalStorage::readOpenAttempts(path(".sidx")), 0u);
}

TEST_F(DictionaryHostTest, InvalidSynonymOrdinalIsAContainedNotFound) {
  const auto entries = numberedEntries(2);
  install(entries, {{"bad-alias", std::numeric_limits<uint32_t>::max()}});
  Dictionary dictionary = openDictionary();
  buildIndex(dictionary);

  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::Found;
  EXPECT_FALSE(dictionary.lookup("bad-alias", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::NotFound);
  EXPECT_TRUE(definition.empty());
}

TEST_F(DictionaryHostTest, QidxAndSidxRebuildIndependentlyAndRejectOldHeaders) {
  const std::vector<Entry> original = {{"apple", "apple definition"}};
  const std::vector<Alias> originalAliases = {{"fruit", 0}};
  install(original, originalAliases);
  Dictionary dictionary = openDictionary();
  buildIndex(dictionary);
  const auto qidxOriginal = HalStorage::readTestFile(path(".qidx"));
  const auto sidxOriginal = HalStorage::readTestFile(path(".sidx"));

  const std::vector<Entry> expanded = {original[0], {"banana", "banana definition"}};
  const auto expandedFiles = encode(expanded, originalAliases);
  HalStorage::registerTestFile(path(".idx"), expandedFiles.idx);
  HalStorage::registerTestFile(path(".dict"), expandedFiles.dict);
  EXPECT_TRUE(dictionary.needsIndex());
  buildIndex(dictionary);
  const auto qidxAfterIdxChange = HalStorage::readTestFile(path(".qidx"));
  EXPECT_NE(qidxAfterIdxChange, qidxOriginal);
  EXPECT_EQ(HalStorage::readTestFile(path(".sidx")), sidxOriginal);
  EXPECT_EQ(HalStorage::writeOpenAttempts(path(".sidx")), 1u);
  EXPECT_FALSE(dictionary.needsIndex());

  const std::vector<Alias> changedAliases = {{"fruit", 0}, {"yellow", 1}};
  const auto changedSyn = encode(expanded, changedAliases).syn;
  HalStorage::registerTestFile(path(".syn"), changedSyn);
  EXPECT_TRUE(dictionary.needsIndex());
  buildIndex(dictionary);
  EXPECT_EQ(HalStorage::readTestFile(path(".qidx")), qidxAfterIdxChange);
  EXPECT_EQ(HalStorage::writeOpenAttempts(path(".qidx")), 2u);
  EXPECT_NE(HalStorage::readTestFile(path(".sidx")), sidxOriginal);
  EXPECT_FALSE(dictionary.needsIndex());

  auto oldVersion = HalStorage::readTestFile(path(".qidx"));
  ASSERT_GE(oldVersion.size(), kSidecarHeaderBytes);
  oldVersion[4] = 1;  // sidecar v1: no entryCount and therefore incompatible
  HalStorage::registerTestFile(path(".qidx"), oldVersion);
  EXPECT_TRUE(dictionary.needsIndex());
  buildIndex(dictionary);
  EXPECT_EQ(readU32Native(HalStorage::readTestFile(path(".qidx")), 4), 2u);
  EXPECT_FALSE(dictionary.needsIndex());

  auto trailingByte = HalStorage::readTestFile(path(".qidx"));
  trailingByte.push_back(0xA5);  // valid header with an impossible file length
  HalStorage::registerTestFile(path(".qidx"), trailingByte);
  EXPECT_TRUE(dictionary.needsIndex());
  buildIndex(dictionary);
  EXPECT_FALSE(dictionary.needsIndex());
}

TEST_F(DictionaryHostTest, TruncatedAndUnreadableBuildsFailSafely) {
  const auto entries = numberedEntries(2);
  const auto files = encode(entries);
  install(files, false);
  Dictionary dictionary = openDictionary();

  auto truncated = files.idx;
  ASSERT_GT(truncated.size(), 1u);
  truncated.pop_back();
  HalStorage::registerTestFile(path(".idx"), truncated);
  Dictionary::IndexResult result = Dictionary::IndexResult::Ok;
  EXPECT_FALSE(dictionary.buildIndex(nullptr, nullptr, &result));
  EXPECT_EQ(result, Dictionary::IndexResult::ReadError);
  EXPECT_FALSE(HalStorage::hasTestFile(path(".qidx")));

  // Restore the complete source and fail after the first scan read. The
  // partially written sidecar must be removed rather than accepted later.
  HalStorage::registerTestFile(path(".idx"), files.idx);
  HalStorage::setReadFailure(path(".idx"), 0);
  result = Dictionary::IndexResult::Ok;
  EXPECT_FALSE(dictionary.buildIndex(nullptr, nullptr, &result));
  EXPECT_EQ(result, Dictionary::IndexResult::ReadError);
  EXPECT_FALSE(HalStorage::hasTestFile(path(".qidx")));
}

TEST_F(DictionaryHostTest, SidecarWriteAndCloseFailuresDoNotLeaveUsableIndexes) {
  const auto entries = numberedEntries(2);
  install(entries);
  Dictionary dictionary = openDictionary();

  HalStorage::failWriteOpen(path(".qidx"));
  Dictionary::IndexResult result = Dictionary::IndexResult::Ok;
  EXPECT_FALSE(dictionary.buildIndex(nullptr, nullptr, &result));
  EXPECT_EQ(result, Dictionary::IndexResult::ReadError);
  EXPECT_FALSE(HalStorage::hasTestFile(path(".qidx")));

  HalStorage::allowWriteOpen(path(".qidx"));
  HalStorage::setCloseFailure(path(".qidx"), true);
  result = Dictionary::IndexResult::Ok;
  EXPECT_FALSE(dictionary.buildIndex(nullptr, nullptr, &result));
  EXPECT_EQ(result, Dictionary::IndexResult::ReadError);
  EXPECT_FALSE(HalStorage::hasTestFile(path(".qidx")));
}

TEST_F(DictionaryHostTest, SampleAndSynonymReadFailuresAreNamedReadErrors) {
  const auto entries = numberedEntries(513);
  install(entries, {{"alternate", 511}});
  Dictionary dictionary = openDictionary();
  buildIndex(dictionary);

  HalStorage::setReadFailure(path(".qidx"), kSidecarHeaderBytes);
  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  EXPECT_FALSE(dictionary.lookup("word0512", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::ReadError);

  // Reopen the sidecar handles with clean qidx data, then fail the synonym
  // stream itself. The alias is present, so a silent clean miss would be wrong.
  HalStorage::registerTestFile(path(".qidx"), HalStorage::readTestFile(path(".qidx")));
  HalStorage::setReadFailure(path(".syn"), 0);
  result = Dictionary::LookupResult::NotFound;
  EXPECT_FALSE(dictionary.lookup("alternate", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::ReadError);
}

TEST_F(DictionaryHostTest, FailedSynonymBuildDisablesSynonymsUntilOpenAndNeverScansSyn) {
  const auto entries = numberedEntries(2);
  install(entries, {{"alternate", 1}});
  Dictionary dictionary = openDictionary();

  // The first allocation is the shared 4KB scan buffer for qidx. Fail the
  // second allocation, which is the independent .sidx pass.
  HostMemory::failAfterAllocations(1);
  Dictionary::IndexResult result = Dictionary::IndexResult::ReadError;
  ASSERT_TRUE(dictionary.buildIndex(nullptr, nullptr, &result));
  EXPECT_EQ(result, Dictionary::IndexResult::Ok);
  EXPECT_TRUE(HalStorage::hasTestFile(path(".qidx")));
  EXPECT_FALSE(HalStorage::hasTestFile(path(".sidx")));

  const size_t synBytesAfterBuild = HalStorage::bytesRead(path(".syn"));
  std::string definition;
  std::string headword;
  Dictionary::LookupResult lookupResult = Dictionary::LookupResult::Found;
  EXPECT_FALSE(dictionary.lookup("alternate", definition, headword, &lookupResult));
  EXPECT_EQ(lookupResult, Dictionary::LookupResult::ReadError);
  EXPECT_EQ(HalStorage::bytesRead(path(".syn")), synBytesAfterBuild);
  const size_t synOpens = HalStorage::readOpenAttempts(path(".syn"));
  EXPECT_FALSE(dictionary.needsIndex());
  EXPECT_TRUE(dictionary.buildIndex());
  EXPECT_FALSE(dictionary.lookup("alternate", definition, headword, &lookupResult));
  EXPECT_EQ(lookupResult, Dictionary::LookupResult::ReadError);
  EXPECT_EQ(HalStorage::readOpenAttempts(path(".syn")), synOpens);
  EXPECT_TRUE(dictionary.lookup("word0001s", definition, headword, &lookupResult));
  EXPECT_EQ(headword, entries[1].headword);

  // open() re-enables the optional source; the next build can create its sidecar.
  HostMemory::reset();
  ASSERT_TRUE(dictionary.open(kFolder));
  ASSERT_TRUE(dictionary.needsIndex());
  buildIndex(dictionary);
  EXPECT_TRUE(dictionary.lookup("alternate", definition, headword, &lookupResult));
  EXPECT_EQ(headword, entries[1].headword);
}

TEST_F(DictionaryHostTest, MainIndexOomUsesNamedLowMemoryResult) {
  install({{"word", "definition"}});
  Dictionary dictionary = openDictionary();
  HostMemory::failNext();

  Dictionary::IndexResult result = Dictionary::IndexResult::ReadError;
  EXPECT_FALSE(dictionary.buildIndex(nullptr, nullptr, &result));
  EXPECT_EQ(result, Dictionary::IndexResult::LowMemory);
}

TEST_F(DictionaryHostTest, DefinitionOomUsesNamedLowMemoryResult) {
  install({{"word", "definition"}});
  Dictionary dictionary = openDictionary();
  buildIndex(dictionary);
  ESP.maxAllocHeap = 1;

  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  EXPECT_FALSE(dictionary.lookup("word", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::LowMemory);
}

TEST(DictionaryLayoutTest, SynonymFlagFitsExistingObjectPadding) {
  EXPECT_EQ(sizeof(Dictionary), sizeof(LegacyDictionaryLayout));
}

TEST_F(DictionaryHostTest, OptionalReadWriteAndTruncationBuildFailuresPreserveOrdinaryLookup) {
  for (int failure = 0; failure < 3; ++failure) {
    SCOPED_TRACE(failure);
    HalStorage::resetTestFiles();
    install({{"dog", "definition"}}, {{"hound", 0}});
    Dictionary dictionary = openDictionary();
    if (failure == 0) HalStorage::setReadFailure(path(".syn"), 0);
    if (failure == 1) HalStorage::setWriteFailure(path(".sidx"), kSidecarHeaderBytes + 2);
    if (failure == 2) {
      auto truncated = HalStorage::readTestFile(path(".syn"));
      truncated.pop_back();
      HalStorage::registerTestFile(path(".syn"), truncated);
    }
    buildIndex(dictionary);
    EXPECT_FALSE(dictionary.needsIndex());
    EXPECT_FALSE(HalStorage::hasTestFile(path(".sidx")));
    std::string definition, headword;
    Dictionary::LookupResult result;
    EXPECT_TRUE(dictionary.lookup("dog", definition, headword, &result));
    EXPECT_TRUE(dictionary.lookup("dogs", definition, headword, &result));
    EXPECT_EQ(headword, "dog");
    EXPECT_FALSE(dictionary.lookup("hound", definition, headword, &result));
    EXPECT_EQ(result, Dictionary::LookupResult::ReadError);
  }
}

TEST_F(DictionaryHostTest, RuntimeSeekAndShortReadsNeverBecomeCleanMisses) {
  install({{"dog", "definition"}}, {{"hound", 0}});
  Dictionary dictionary = openDictionary();
  buildIndex(dictionary);
  std::string definition, headword;
  Dictionary::LookupResult result;
  HalStorage::setSeekFailure(path(".syn"), true);
  EXPECT_FALSE(dictionary.lookup("hound", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::ReadError);
  HalStorage::setSeekFailure(path(".syn"), false);
  HalStorage::setReadFailure(path(".syn"), 7);  // short BE32 ordinal
  EXPECT_FALSE(dictionary.lookup("hound", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::ReadError);
  HalStorage::setReadFailure(path(".syn"), std::numeric_limits<size_t>::max());
  HalStorage::setSeekFailure(path(".idx"), true);
  EXPECT_FALSE(dictionary.lookup("hound", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::ReadError);
  HalStorage::setSeekFailure(path(".idx"), false);
  HalStorage::setReadFailure(path(".idx"), 5);  // short definition offset/length
  EXPECT_FALSE(dictionary.lookup("hound", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::ReadError);
}

TEST_F(DictionaryHostTest, MissingSidecarsNeverCauseAnUnindexedSynonymScan) {
  install({{"dog", "definition"}}, {{"hound", 0}});
  Dictionary dictionary = openDictionary();
  std::string definition, headword;
  Dictionary::LookupResult result;
  EXPECT_TRUE(dictionary.lookup("dog", definition, headword, &result));
  EXPECT_FALSE(dictionary.lookup("hound", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::ReadError);
  EXPECT_EQ(HalStorage::bytesRead(path(".syn")), 0u);
  buildIndex(dictionary);
  Storage.remove(path(".qidx").c_str());
  EXPECT_TRUE(dictionary.lookup("hound", definition, headword, &result));
  EXPECT_EQ(headword, "dog");
}

TEST_F(DictionaryHostTest, EmptySynonymFileAndOutOfRangeOrdinalWithoutQidxAreSafe) {
  install({{"dog", "definition"}}, std::vector<Alias>{});
  Dictionary dictionary = openDictionary();
  buildIndex(dictionary);
  EXPECT_FALSE(dictionary.needsIndex());
  std::string definition, headword;
  Dictionary::LookupResult result;
  EXPECT_FALSE(dictionary.lookup("hound", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::NotFound);

  install({{"dog", "definition"}}, {{"hound", 1}});
  ASSERT_TRUE(dictionary.open(kFolder));
  buildIndex(dictionary);
  EXPECT_FALSE(dictionary.lookup("hound", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::NotFound);
  Storage.remove(path(".qidx").c_str());
  EXPECT_FALSE(dictionary.lookup("hound", definition, headword, &result));
  EXPECT_EQ(result, Dictionary::LookupResult::NotFound);
}

TEST_F(DictionaryHostTest, HtmlDefinitionTypeMatchesExactIfoLineAndResetsOnOpen) {
  install(std::vector<Entry>{{"word", "<b>definition</b>"}});
  const std::string html = "StarDict's dict ifo file\nversion=2.4.2\nsametypesequence=h\r\n";
  HalStorage::registerTestFile(path(".ifo"), {html.begin(), html.end()});
  auto dictionary = openDictionary();
  EXPECT_TRUE(dictionary.hasHtmlDefinitions());
  for (const std::string value :
       {"sametypesequence=m\n", "sametypesequence=hm\n", "description=sametypesequence=h\n"}) {
    HalStorage::registerTestFile(path(".ifo"), {value.begin(), value.end()});
    ASSERT_TRUE(dictionary.open(kFolder));
    EXPECT_FALSE(dictionary.hasHtmlDefinitions());
  }
}
