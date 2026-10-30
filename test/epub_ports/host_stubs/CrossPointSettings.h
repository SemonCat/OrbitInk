#pragma once
struct CrossPointSettings {
  enum SLEEP_SCREEN_COVER_MODE { FIT, CROP };
  int sleepScreenCoverMode = FIT;
  int getReaderFontId() const { return 1; }
  float getReaderLineCompression() const { return 1.0f; }
  bool extraParagraphSpacing = false, hyphenationEnabled = false, focusReadingEnabled = false;
  unsigned char paragraphAlignment = 1;
};
inline CrossPointSettings SETTINGS;
