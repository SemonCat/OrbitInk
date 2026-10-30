#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "EspImageBuilder.h"
#include "lib/FirmwareUpdate/EspImageValidator.h"

using esp_image::Status;
using esp_image::StreamingValidator;

namespace {
constexpr size_t PARTITION = 0x640000;

Status validate(const std::vector<uint8_t>& img, size_t chunk, size_t maxSize = PARTITION, uint16_t chip = 0x0005) {
  StreamingValidator v(maxSize, chip);
  for (size_t off = 0; off < img.size(); off += chunk) {
    const size_t n = std::min(chunk, img.size() - off);
    if (!v.feed(img.data() + off, n)) break;
  }
  return v.finish();
}
}  // namespace

TEST(EspImageValidator, AcceptsValidImageAtAnyChunking) {
  const auto img = buildEspImage({40000, 30000, 7});
  for (size_t chunk : {1u, 7u, 24u, 4096u, 1u << 20}) {
    EXPECT_EQ(validate(img, chunk), Status::OK) << chunk;
  }
}

TEST(EspImageValidator, AcceptsImageWithoutShaTrailer) {
  EXPECT_EQ(validate(buildEspImage({70000}, /*hashAppended=*/false), 4096), Status::OK);
}

TEST(EspImageValidator, AcceptsAnyChipWhenRunningChipUnknown) {
  EXPECT_EQ(validate(buildEspImage({70000}, true, 0x0009), 4096, PARTITION, StreamingValidator::ANY_CHIP), Status::OK);
}

TEST(EspImageValidator, RejectsBadMagicFromFirstByte) {
  auto img = buildEspImage({70000});
  img[0] = 0x00;
  StreamingValidator v(PARTITION, 0x0005);
  EXPECT_FALSE(v.feed(img.data(), 1));
  EXPECT_EQ(v.failure(), Status::BAD_MAGIC);
}

TEST(EspImageValidator, RejectsWrongChipFromHeader) {
  const auto img = buildEspImage({70000}, true, 0x0009);  // ESP32-S3 image on a C3
  StreamingValidator v(PARTITION, 0x0005);
  EXPECT_FALSE(v.feed(img.data(), 24));
  EXPECT_EQ(v.failure(), Status::BAD_CHIP);
}

TEST(EspImageValidator, RejectsOversizeAsSoonAsLimitPassed) {
  const auto img = buildEspImage({70000});
  StreamingValidator v(img.size() - 1, 0x0005);
  EXPECT_TRUE(v.feed(img.data(), img.size() - 1));
  EXPECT_FALSE(v.feed(img.data() + img.size() - 1, 1));
  EXPECT_EQ(v.finish(), Status::TOO_LARGE);
}

TEST(EspImageValidator, RejectsTooSmall) { EXPECT_EQ(validate(buildEspImage({1000}), 4096), Status::TOO_SMALL); }

TEST(EspImageValidator, TruncationInSegmentsIsBadSegments) {
  auto img = buildEspImage({40000, 40000});
  img.resize(70000);
  EXPECT_EQ(validate(img, 4096), Status::BAD_SEGMENTS);
}

TEST(EspImageValidator, TruncationInShaTrailerIsBadSize) {
  auto img = buildEspImage({70000});
  img.resize(img.size() - 5);
  EXPECT_EQ(validate(img, 4096), Status::BAD_SIZE);
}

TEST(EspImageValidator, TrailingGarbageIsBadSize) {
  auto img = buildEspImage({70000});
  img.push_back(0);
  EXPECT_EQ(validate(img, 4096), Status::BAD_SIZE);
}

TEST(EspImageValidator, FlippedDataByteIsBadChecksum) {
  auto img = buildEspImage({70000}, /*hashAppended=*/false);
  img[24 + 8 + 100] ^= 0x01;
  EXPECT_EQ(validate(img, 4096), Status::BAD_CHECKSUM);
}

TEST(EspImageValidator, FlippedPadByteIsBadSha) {
  auto img = buildEspImage({70001});
  // A non-checksum pad byte is covered only by the SHA trailer.
  const size_t padStart = 24 + 8 + 70001;
  img[padStart] ^= 0x01;
  EXPECT_EQ(validate(img, 4096), Status::BAD_SHA);
}

TEST(EspImageValidator, FinishIsIdempotent) {
  StreamingValidator v(PARTITION, 0x0005);
  const auto img = buildEspImage({70000});
  v.feed(img.data(), img.size());
  EXPECT_EQ(v.finish(), Status::OK);
  EXPECT_EQ(v.finish(), Status::OK);
  EXPECT_FALSE(v.feed(img.data(), 1));
}
