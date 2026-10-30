#include "BookOrbitBookmarks.h"

#include <MD5Builder.h>

#include <cstring>

bool bookOrbitBookmarkKey(const char* datetime, const char* pos,
                         char (&outKey)[BookOrbitBookmarkKeyRow::DIGEST_SIZE]) {
  if (!datetime || !*datetime || !pos || !*pos) return false;

  // md5(datetime | pos), fed in three pieces so neither field has to be concatenated into a
  // temporary first.
  MD5Builder md5;
  md5.begin();
  md5.add(datetime);
  md5.add("|");
  md5.add(pos);
  md5.calculate();

  const String hex = md5.toString();
  if (strlen(hex.c_str()) + 1 != BookOrbitBookmarkKeyRow::DIGEST_SIZE) return false;
  memcpy(outKey, hex.c_str(), BookOrbitBookmarkKeyRow::DIGEST_SIZE);
  return true;
}
