#include "config/DashboardLayoutConfig.h"

#include <Preferences.h>

namespace {

static constexpr char kPrefsNamespace[] = "dashLayout";
static constexpr char kKeyLeft[] = "left";
static constexpr char kKeyCenter[] = "center";
static constexpr char kKeyRight[] = "right";
static constexpr char kKeyVersion[] = "ver";
static constexpr uint8_t kVersion = 1;

bool isValidWidget(uint8_t value) {
  return value <= static_cast<uint8_t>(DashboardWidgetKind::Compass);
}

}  // namespace

bool DashboardLayoutConfig::begin() {
  if (!load()) {
    resetDefault();
    save();
    return false;
  }
  return true;
}

bool DashboardLayoutConfig::load() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    return false;
  }

  const uint8_t version = prefs.getUChar(kKeyVersion, 0);
  const uint8_t left = prefs.getUChar(kKeyLeft, 255);
  const uint8_t center = prefs.getUChar(kKeyCenter, 255);
  const uint8_t right = prefs.getUChar(kKeyRight, 255);
  prefs.end();

  if (version != kVersion || !isValidWidget(left) || !isValidWidget(center) || !isValidWidget(right)) {
    return false;
  }

  DashboardWidgetKind loaded[3] = {
      static_cast<DashboardWidgetKind>(left),
      static_cast<DashboardWidgetKind>(center),
      static_cast<DashboardWidgetKind>(right),
  };
  if (!validSlots(loaded)) {
    return false;
  }

  slots_[0] = loaded[0];
  slots_[1] = loaded[1];
  slots_[2] = loaded[2];
  return true;
}

bool DashboardLayoutConfig::save() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    Serial.println("Dashboard: falha ao abrir NVS para salvar layout.");
    return false;
  }

  prefs.putUChar(kKeyVersion, kVersion);
  prefs.putUChar(kKeyLeft, static_cast<uint8_t>(slots_[0]));
  prefs.putUChar(kKeyCenter, static_cast<uint8_t>(slots_[1]));
  prefs.putUChar(kKeyRight, static_cast<uint8_t>(slots_[2]));
  prefs.end();
  Serial.println("Dashboard: layout da tela inicial salvo.");
  return true;
}

void DashboardLayoutConfig::resetDefault() {
  slots_[0] = DashboardWidgetKind::Thermal;
  slots_[1] = DashboardWidgetKind::Vario;
  slots_[2] = DashboardWidgetKind::Compass;
}

DashboardWidgetKind DashboardLayoutConfig::widgetForSlot(DashboardSlot slot) const {
  const uint8_t index = static_cast<uint8_t>(slot);
  return index < 3 ? slots_[index] : DashboardWidgetKind::Vario;
}

DashboardSlot DashboardLayoutConfig::slotForWidget(DashboardWidgetKind widget) const {
  for (uint8_t i = 0; i < 3; ++i) {
    if (slots_[i] == widget) {
      return static_cast<DashboardSlot>(i);
    }
  }
  return DashboardSlot::Center;
}

void DashboardLayoutConfig::moveWidgetToSlot(DashboardWidgetKind widget, DashboardSlot targetSlot) {
  const DashboardSlot currentSlot = slotForWidget(widget);
  const uint8_t current = static_cast<uint8_t>(currentSlot);
  const uint8_t target = static_cast<uint8_t>(targetSlot);
  if (current >= 3 || target >= 3 || current == target) {
    return;
  }

  const DashboardWidgetKind replaced = slots_[target];
  slots_[target] = widget;
  slots_[current] = replaced;
}

void DashboardLayoutConfig::setCenterWidget(DashboardWidgetKind widget) {
  moveWidgetToSlot(widget, DashboardSlot::Center);
}

bool DashboardLayoutConfig::validSlots(const DashboardWidgetKind slots[3]) const {
  bool seen[3] = {};
  for (uint8_t i = 0; i < 3; ++i) {
    const uint8_t value = static_cast<uint8_t>(slots[i]);
    if (!isValidWidget(value) || seen[value]) {
      return false;
    }
    seen[value] = true;
  }
  return true;
}

const char* dashboardWidgetLabel(DashboardWidgetKind widget) {
  switch (widget) {
    case DashboardWidgetKind::Thermal:
      return "ASSISTENTE TERMICA";
    case DashboardWidgetKind::Vario:
      return "VARIO";
    case DashboardWidgetKind::Compass:
      return "RUMO / VENTO";
    default:
      return "WIDGET";
  }
}
