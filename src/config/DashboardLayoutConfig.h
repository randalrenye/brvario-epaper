#pragma once

#include <Arduino.h>
#include <stdint.h>

enum class DashboardWidgetKind : uint8_t {
  Thermal = 0,
  Vario = 1,
  Compass = 2,
};

enum class DashboardSlot : uint8_t {
  Left = 0,
  Center = 1,
  Right = 2,
};

class DashboardLayoutConfig {
 public:
  bool begin();
  bool save();
  void resetDefault();

  DashboardWidgetKind widgetForSlot(DashboardSlot slot) const;
  DashboardSlot slotForWidget(DashboardWidgetKind widget) const;
  void moveWidgetToSlot(DashboardWidgetKind widget, DashboardSlot targetSlot);
  void setCenterWidget(DashboardWidgetKind widget);

 private:
  DashboardWidgetKind slots_[3] = {
      DashboardWidgetKind::Thermal,
      DashboardWidgetKind::Vario,
      DashboardWidgetKind::Compass,
  };

  bool load();
  bool validSlots(const DashboardWidgetKind slots[3]) const;
};

const char* dashboardWidgetLabel(DashboardWidgetKind widget);
