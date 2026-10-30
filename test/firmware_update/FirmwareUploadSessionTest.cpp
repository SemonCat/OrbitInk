#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "EspImageBuilder.h"
#include "lib/FirmwareUpdate/FirmwareUploadSession.h"

using firmware_upload::Error;
using firmware_upload::Session;
using firmware_upload::State;

namespace {
constexpr size_t PARTITION = 0x640000;

// In-memory SD: records every operation so tests can assert nothing unexpected is written.
struct FakeSink : firmware_upload::Sink {
  bool partExists = false;
  bool stagedExists = false;
  bool partOpen = false;
  std::vector<uint8_t> part;
  std::vector<uint8_t> staged;
  std::vector<std::string> ops;
  bool failOpen = false;
  size_t failWriteAfter = SIZE_MAX;  // fail once this many bytes were written
  bool failPromote = false;

  bool openPart() override {
    ops.push_back("open");
    if (failOpen) return false;
    part.clear();
    partExists = partOpen = true;
    return true;
  }
  bool write(const uint8_t* data, size_t len) override {
    ops.push_back("write");
    if (!partOpen || part.size() + len > failWriteAfter) return false;
    part.insert(part.end(), data, data + len);
    return true;
  }
  bool closePart() override {
    ops.push_back("close");
    partOpen = false;
    return true;
  }
  void removePart() override {
    ops.push_back("removePart");
    partExists = false;
    part.clear();
  }
  bool promote() override {
    ops.push_back("promote");
    if (failPromote || !partExists) return false;
    staged = part;
    stagedExists = true;
    partExists = false;
    part.clear();
    return true;
  }
  void removeStaged() override {
    ops.push_back("removeStaged");
    stagedExists = false;
    staged.clear();
  }
  size_t count(const std::string& op) const { return std::count(ops.begin(), ops.end(), op); }
};

bool feedAll(Session& s, const std::vector<uint8_t>& data, size_t chunk = 1460) {
  for (size_t off = 0; off < data.size(); off += chunk) {
    if (!s.write(data.data() + off, std::min(chunk, data.size() - off))) return false;
  }
  return true;
}
}  // namespace

TEST(FirmwareUploadSession, ValidUploadIsStagedWithHash) {
  FakeSink sink;
  Session s(sink, PARTITION, 0x0005);
  const auto img = buildEspImage({90000, 5000});
  ASSERT_TRUE(s.begin(img.size()));
  ASSERT_TRUE(feedAll(s, img));
  ASSERT_TRUE(s.end());
  EXPECT_EQ(s.state(), State::STAGED);
  EXPECT_EQ(s.stagedSize(), img.size());
  EXPECT_EQ(sink.staged, img);
  EXPECT_FALSE(sink.partExists);
  char hex[65];
  firmware_upload::shaToHex(sha256Of(img).data(), hex);
  EXPECT_TRUE(s.stagedShaMatches(hex));
  hex[0] = hex[0] == '0' ? '1' : '0';
  EXPECT_FALSE(s.stagedShaMatches(hex));
}

TEST(FirmwareUploadSession, NonImageNeverTouchesStorage) {
  FakeSink sink;
  Session s(sink, PARTITION, 0x0005);
  std::vector<uint8_t> epub(100000, 'P');
  ASSERT_TRUE(s.begin(epub.size()));
  EXPECT_FALSE(s.write(epub.data(), 1460));
  EXPECT_EQ(s.lastError(), Error::INVALID);
  EXPECT_EQ(s.imageStatus(), esp_image::Status::BAD_MAGIC);
  EXPECT_EQ(sink.count("open"), 0u);
  EXPECT_EQ(sink.count("write"), 0u);
  EXPECT_EQ(s.state(), State::IDLE);
  // Remaining body chunks of the same request are ignored.
  EXPECT_FALSE(s.write(epub.data(), 1460));
  EXPECT_FALSE(s.end());
}

TEST(FirmwareUploadSession, WrongChipRejectedBeforeOpen) {
  FakeSink sink;
  Session s(sink, PARTITION, 0x0005);
  const auto img = buildEspImage({90000}, true, 0x0009);
  ASSERT_TRUE(s.begin(0));
  EXPECT_FALSE(s.write(img.data(), 1460));
  EXPECT_EQ(s.imageStatus(), esp_image::Status::BAD_CHIP);
  EXPECT_EQ(sink.count("open"), 0u);
}

TEST(FirmwareUploadSession, SplitHeaderIsCheckedBeforeStorageOpens) {
  FakeSink sink;
  Session s(sink, PARTITION, 0x0005);
  const auto img = buildEspImage({90000}, true, 0x0009);
  ASSERT_TRUE(s.begin(0));
  for (size_t i = 0; i < 13; ++i) ASSERT_TRUE(s.write(&img[i], 1));
  EXPECT_EQ(sink.count("open"), 0u);
  EXPECT_FALSE(s.write(&img[13], 1));
  EXPECT_EQ(s.imageStatus(), esp_image::Status::BAD_CHIP);
  EXPECT_EQ(sink.count("write"), 0u);
  EXPECT_EQ(sink.count("open"), 0u);
}

TEST(FirmwareUploadSession, IncompleteHeaderNeverOpensStorage) {
  FakeSink sink;
  Session s(sink, PARTITION, 0x0005);
  const auto img = buildEspImage({90000});
  ASSERT_TRUE(s.begin(0) && s.write(img.data(), 12));
  EXPECT_FALSE(s.end());
  EXPECT_EQ(s.lastError(), Error::INVALID);
  EXPECT_EQ(sink.count("open"), 0u);
}

TEST(FirmwareUploadSession, DeclaredOversizeRefusedWithoutAnyIo) {
  FakeSink sink;
  Session s(sink, PARTITION, 0x0005);
  EXPECT_FALSE(s.begin(PARTITION + 1));
  EXPECT_EQ(s.lastError(), Error::TOO_LARGE);
  EXPECT_TRUE(sink.ops.empty());
}

TEST(FirmwareUploadSession, StreamedOversizeRemovesPartial) {
  FakeSink sink;
  const auto img = buildEspImage({90000});
  Session s(sink, img.size() - 100, 0x0005);
  ASSERT_TRUE(s.begin(0));  // size unknown up front
  EXPECT_FALSE(feedAll(s, img));
  EXPECT_EQ(s.lastError(), Error::TOO_LARGE);
  EXPECT_FALSE(sink.partExists);
  EXPECT_FALSE(sink.stagedExists);
}

TEST(FirmwareUploadSession, TruncatedUploadRejectedAndCleaned) {
  FakeSink sink;
  Session s(sink, PARTITION, 0x0005);
  auto img = buildEspImage({200000});
  img.resize(img.size() / 2);  // still above the 64 KiB minimum, so the segment walk decides
  ASSERT_TRUE(s.begin(0));
  ASSERT_TRUE(feedAll(s, img));
  EXPECT_FALSE(s.end());
  EXPECT_EQ(s.lastError(), Error::INVALID);
  EXPECT_EQ(s.imageStatus(), esp_image::Status::BAD_SEGMENTS);
  EXPECT_FALSE(sink.partExists);
  EXPECT_FALSE(sink.stagedExists);
  EXPECT_EQ(sink.count("promote"), 0u);
}

TEST(FirmwareUploadSession, AbortMidTransferRemovesPartialOnly) {
  FakeSink sink;
  Session s(sink, PARTITION, 0x0005);
  const auto img = buildEspImage({90000});
  ASSERT_TRUE(s.begin(img.size()));
  ASSERT_TRUE(s.write(img.data(), 8192));
  s.abort();
  EXPECT_EQ(s.state(), State::IDLE);
  EXPECT_EQ(s.lastError(), Error::ABORTED);
  EXPECT_FALSE(sink.partExists);
  EXPECT_EQ(sink.count("promote"), 0u);
}

TEST(FirmwareUploadSession, AbortWithoutStartIsHarmless) {
  FakeSink sink;
  Session s(sink, PARTITION, 0x0005);
  s.abort();
  EXPECT_TRUE(sink.ops.empty());
}

TEST(FirmwareUploadSession, OverlappingBeginIsBusyAndKeepsFirst) {
  FakeSink sink;
  Session s(sink, PARTITION, 0x0005);
  const auto img = buildEspImage({90000});
  ASSERT_TRUE(s.begin(img.size()));
  ASSERT_TRUE(s.write(img.data(), 4096));
  const auto opsBefore = sink.ops.size();
  EXPECT_FALSE(s.begin(img.size()));
  EXPECT_EQ(s.lastError(), Error::BUSY);
  EXPECT_EQ(sink.ops.size(), opsBefore);
  ASSERT_TRUE(s.write(img.data() + 4096, img.size() - 4096));
  EXPECT_TRUE(s.end());
}

TEST(FirmwareUploadSession, RepeatUploadReplacesStagedImage) {
  FakeSink sink;
  Session s(sink, PARTITION, 0x0005);
  const auto first = buildEspImage({90000});
  const auto second = buildEspImage({80000, 1234});
  ASSERT_TRUE(s.begin(0) && feedAll(s, first) && s.end());
  ASSERT_TRUE(s.begin(0) && feedAll(s, second) && s.end());
  EXPECT_EQ(sink.staged, second);
  EXPECT_EQ(s.stagedSize(), second.size());
}

TEST(FirmwareUploadSession, FailedRepeatLeavesNothingStaged) {
  FakeSink sink;
  Session s(sink, PARTITION, 0x0005);
  const auto good = buildEspImage({90000});
  ASSERT_TRUE(s.begin(0) && feedAll(s, good) && s.end());
  auto bad = good;
  bad.resize(bad.size() - 1);
  ASSERT_TRUE(s.begin(0));
  feedAll(s, bad);
  EXPECT_FALSE(s.end());
  EXPECT_FALSE(sink.stagedExists);  // never a stale image that the page no longer describes
  EXPECT_EQ(s.state(), State::IDLE);
}

TEST(FirmwareUploadSession, WriteFailureCleansUp) {
  FakeSink sink;
  sink.failWriteAfter = 8192;
  Session s(sink, PARTITION, 0x0005);
  const auto img = buildEspImage({90000});
  ASSERT_TRUE(s.begin(0));
  EXPECT_FALSE(feedAll(s, img));
  EXPECT_EQ(s.lastError(), Error::IO);
  EXPECT_FALSE(sink.partExists);
}

TEST(FirmwareUploadSession, OpenFailureIsIo) {
  FakeSink sink;
  sink.failOpen = true;
  Session s(sink, PARTITION, 0x0005);
  const auto img = buildEspImage({90000});
  ASSERT_TRUE(s.begin(0));
  EXPECT_FALSE(s.write(img.data(), 4096));
  EXPECT_EQ(s.lastError(), Error::IO);
}

TEST(FirmwareUploadSession, PromoteFailureIsIo) {
  FakeSink sink;
  sink.failPromote = true;
  Session s(sink, PARTITION, 0x0005);
  const auto img = buildEspImage({90000});
  ASSERT_TRUE(s.begin(0) && feedAll(s, img));
  EXPECT_FALSE(s.end());
  EXPECT_EQ(s.lastError(), Error::IO);
  EXPECT_EQ(s.state(), State::IDLE);
}

TEST(FirmwareUploadSession, DiscardRemovesStaged) {
  FakeSink sink;
  Session s(sink, PARTITION, 0x0005);
  const auto img = buildEspImage({90000});
  ASSERT_TRUE(s.begin(0) && feedAll(s, img) && s.end());
  s.discard();
  EXPECT_EQ(s.state(), State::IDLE);
  EXPECT_FALSE(sink.stagedExists);
  EXPECT_EQ(s.stagedSha(), nullptr);
}

TEST(FirmwareUploadSession, ExpectedHashAndSizeEnforced) {
  const auto img = buildEspImage({90000});
  const auto sha = sha256Of(img);
  {
    FakeSink sink;
    Session s(sink, PARTITION, 0x0005);
    ASSERT_TRUE(s.begin(0, sha.data(), img.size()) && feedAll(s, img));
    EXPECT_TRUE(s.end());
  }
  {
    FakeSink sink;
    Session s(sink, PARTITION, 0x0005);
    auto wrong = sha;
    wrong[0] ^= 1;
    ASSERT_TRUE(s.begin(0, wrong.data(), img.size()) && feedAll(s, img));
    EXPECT_FALSE(s.end());
    EXPECT_EQ(s.lastError(), Error::HASH_MISMATCH);
    EXPECT_FALSE(sink.stagedExists);
    EXPECT_FALSE(sink.partExists);
  }
  {
    FakeSink sink;
    Session s(sink, PARTITION, 0x0005);
    ASSERT_TRUE(s.begin(0, sha.data(), img.size() - 10));
    EXPECT_FALSE(feedAll(s, img));
    EXPECT_EQ(s.lastError(), Error::SIZE_MISMATCH);
  }
  {
    FakeSink sink;
    Session s(sink, PARTITION, 0x0005);
    ASSERT_TRUE(s.begin(0, sha.data(), img.size() + 10) && feedAll(s, img));
    EXPECT_FALSE(s.end());
    EXPECT_EQ(s.lastError(), Error::SIZE_MISMATCH);
  }
}

TEST(FirmwareUploadSession, HexRoundTripIsStrict) {
  uint8_t sha[32];
  for (int i = 0; i < 32; i++) sha[i] = static_cast<uint8_t>(i * 7);
  char hex[65];
  firmware_upload::shaToHex(sha, hex);
  uint8_t back[32];
  ASSERT_TRUE(firmware_upload::hexToSha(hex, back));
  EXPECT_EQ(0, memcmp(sha, back, 32));
  EXPECT_FALSE(firmware_upload::hexToSha("abc", back));
  std::string longer = std::string(hex) + "0";
  EXPECT_FALSE(firmware_upload::hexToSha(longer.c_str(), back));
  hex[10] = 'g';
  EXPECT_FALSE(firmware_upload::hexToSha(hex, back));
  EXPECT_FALSE(firmware_upload::hexToSha(nullptr, back));
}
