#pragma once

#include "data/VarioData.h"
#include "display/EpdDisplay.h"

class TrendWidget {
 public:
  explicit TrendWidget(const Rect_t& bounds) : bounds_(bounds) {}
  void render(EpdDisplay& display, const VarioData& data);
  void renderStatic(EpdDisplay& display);
  void renderDynamic(EpdDisplay& display, const VarioData& data);
  Rect_t bounds() const { return bounds_; }
  Rect_t dynamicBounds() const;

 private:
  Rect_t bounds_;
};
