#pragma once
struct UITheme {
  static UITheme& getInstance() { static UITheme theme; return theme; }
  int getStatusBarHeight() { return 20; }
};
