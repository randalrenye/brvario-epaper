#pragma once

#include "data/VarioData.h"
#include "display/EpdDisplay.h"

class VarioGaugeWidget {
 public:
  enum class SideInfoMode : uint8_t {
    None,
    Altitude,
    Speed,
  };

  explicit VarioGaugeWidget(const Rect_t& bounds) : bounds_(bounds) {}
  void render(EpdDisplay& display, const VarioData& data);
  void renderStatic(EpdDisplay& display);
  void renderDynamic(EpdDisplay& display, const VarioData& data);
  Rect_t bounds() const { return bounds_; }
  void setBounds(const Rect_t& bounds) { bounds_ = bounds; }
  void setSideInfoMode(SideInfoMode mode) { sideInfoMode_ = mode; }
  Rect_t dynamicBounds() const;
  Rect_t needleBounds() const;
  Rect_t valueBounds() const;

 private:
  Rect_t bounds_;
  SideInfoMode sideInfoMode_ = SideInfoMode::None;

  float valueToAngleRad(float value) const;
  Rect_t gaugeArea() const;
  Rect_t topSideInfoBounds() const;
  Rect_t bottomSideInfoBounds() const;
  int32_t centerX() const;
  int32_t centerY() const;
  int32_t radius() const;
  bool compact() const;
  bool sideInfoActive() const;
};
