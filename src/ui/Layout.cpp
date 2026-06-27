#include "Layout.h"

LayoutRegions Layout::horizontal() {
  LayoutRegions r;
  r.screen = {8, 8, EPD_WIDTH - 16, EPD_HEIGHT - 16};
  r.header = {8, 8, EPD_WIDTH - 16, 42};
  r.info = {8, 50, 270, 482};
  r.vario = {278, 50, 400, 422};
  r.speed = {678, 50, 274, 482};
  r.trend = {278, 472, 400, 60};
  r.footerButtons = {278, 472, 400, 60};
  return r;
}
