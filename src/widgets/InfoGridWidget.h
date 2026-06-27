#pragma once

#include "data/VarioData.h"
#include "display/EpdDisplay.h"

class InfoGridWidget {
 public:
  enum class Mode : uint8_t {
    Full,
    ThermalOnly,
  };

  explicit InfoGridWidget(const Rect_t& bounds) : bounds_(bounds) {}
  void render(EpdDisplay& display, const VarioData& data);
  void renderStatic(EpdDisplay& display);
  void renderDynamic(EpdDisplay& display, const VarioData& data);
  Rect_t bounds() const { return bounds_; }
  void setBounds(const Rect_t& bounds) { bounds_ = bounds; }
  void setMode(Mode mode) { mode_ = mode; }
  Rect_t dynamicBounds() const;
  Rect_t cellValueBounds(uint8_t index) const;
  uint8_t cellCount() const { return 3; }

 private:
  Rect_t bounds_;
  Mode mode_ = Mode::Full;

  Rect_t blockBounds(uint8_t index) const;
  void drawBlockStatic(EpdDisplay& display, const Rect_t& block, const char* label);
  void drawBlockValue(EpdDisplay& display, const Rect_t& block, const char* value);
  void drawThermalStatic(EpdDisplay& display, const Rect_t& block);
  void drawThermalDynamic(EpdDisplay& display, const Rect_t& block, const VarioData& data);
};
