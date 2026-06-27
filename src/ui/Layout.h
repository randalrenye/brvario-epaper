#pragma once

#include "epd_driver.h"

struct LayoutRegions {
  Rect_t screen;
  Rect_t header;
  Rect_t vario;
  Rect_t info;
  Rect_t speed;
  Rect_t trend;
  Rect_t footerButtons;
};

class Layout {
 public:
  static LayoutRegions horizontal();
};
