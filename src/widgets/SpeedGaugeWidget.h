#pragma once

#include "data/VarioData.h"
#include "display/EpdDisplay.h"

class SpeedGaugeWidget {
 public:
  enum class Mode : uint8_t {
    Full,
    CompassOnly,
  };

  explicit SpeedGaugeWidget(const Rect_t& bounds) : bounds_(bounds) {}
  void render(EpdDisplay& display, const VarioData& data);
  void renderStatic(EpdDisplay& display);
  void renderDynamic(EpdDisplay& display, const VarioData& data);
  Rect_t bounds() const { return bounds_; }
  void setBounds(const Rect_t& bounds) { bounds_ = bounds; }
  void setMode(Mode mode) { mode_ = mode; }
  Rect_t dynamicBounds() const;
  Rect_t compassBounds() const;
  Rect_t groundSpeedValueBounds() const;
  Rect_t windSpeedValueBounds() const;

 private:
  Rect_t bounds_;
  Mode mode_ = Mode::Full;
};
