#pragma once

#include "display/EpdDisplay.h"
#include "navigation/ThermalCycleBeta.h"

class ThermalCyclePage {
 public:
  void render(EpdDisplay& display, const Rect_t& screen, int32_t contentBottomY, const ThermalCycleBeta::Snapshot& state);

 private:
  static void formatMinutes(uint32_t valueMs, char* out, size_t outSize);
  static void formatSignedMinutes(int32_t valueMs, char* out, size_t outSize);
  static void formatClock(uint32_t valueMs, char* out, size_t outSize);
};
