#include "HeaderWidget.h"

#include <stdio.h>

#include "config/AppConfig.h"

namespace {

void formatClock(char* text, size_t size, uint32_t secondsOfDay) {
  const uint32_t hours = (secondsOfDay / 3600UL) % 24UL;
  const uint32_t minutes = (secondsOfDay / 60UL) % 60UL;
  snprintf(text, size, "%02lu:%02lu", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
}

void drawBatteryIcon(EpdDisplay& display, int32_t x, int32_t y, uint8_t percent, bool charging) {
  display.drawRect({x, y, 42, 20}, AppConfig::kBlack);
  display.fillRect({x + 42, y + 6, 4, 8}, AppConfig::kBlack);

  if (charging) {
    display.fillRect({x + 3, y + 3, 36, 14}, AppConfig::kWhite);
    display.fillTriangle(x + 23, y + 3, x + 13, y + 12, x + 22, y + 12, AppConfig::kBlack);
    display.fillTriangle(x + 20, y + 8, x + 30, y + 8, x + 16, y + 18, AppConfig::kBlack);
    return;
  }

  const uint8_t bars = percent >= 80 ? 4 : percent >= 55 ? 3 : percent >= 30 ? 2 : percent >= 10 ? 1 : 0;
  for (uint8_t i = 0; i < 4; ++i) {
    const int32_t barX = x + 5 + static_cast<int32_t>(i) * 9;
    const uint8_t color = i < bars ? AppConfig::kBlack : AppConfig::kWhite;
    display.fillRect({barX, y + 4, 6, 12}, color);
    display.drawRect({barX, y + 4, 6, 12}, AppConfig::kBlack);
  }
}

void drawBluetoothIcon(EpdDisplay& display, int32_t x, int32_t y, bool connected) {
  display.drawRect({x, y, 44, 24}, AppConfig::kBlack);
  display.drawSmallTextBoldAligned("BT", x + 22, y + 5, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  if (connected) {
    display.fillRect({x + 35, y + 4, 5, 5}, AppConfig::kBlack);
  }
}

void drawGpsStatus(EpdDisplay& display, int32_t x, int32_t y, bool fix, uint8_t satellites) {
  display.drawRect({x, y, 116, 24}, AppConfig::kBlack);

  const char* status = fix ? "GPS ON" : "GPS OFF";
  display.drawSmallTextBoldAligned(status, x + 47, y + 5, 2, AppConfig::kBlack, EpdDisplay::Align::Center);

  char satText[4];
  snprintf(satText, sizeof(satText), "%02u", satellites);
  display.drawSmallTextBoldAligned(satText, x + 100, y + 5, 2, AppConfig::kBlack, EpdDisplay::Align::Center);

  if (!fix) {
    display.drawLine(x + 4, y + 21, x + 112, y + 3, AppConfig::kBlack);
  }
}

void drawWifiIcon(EpdDisplay& display, int32_t x, int32_t y) {
  display.drawRect({x, y, 56, 24}, AppConfig::kBlack);
  display.drawSmallTextBoldAligned("WIFI", x + 28, y + 5, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  display.fillRect({x + 47, y + 4, 5, 5}, AppConfig::kBlack);
}

}  // namespace

void HeaderWidget::render(EpdDisplay& display, const VarioData& data) {
  renderStatic(display);
  renderDynamic(display, data);
}

void HeaderWidget::renderStatic(EpdDisplay& display) {
  display.fillRect(bounds_, AppConfig::kWhite);

  display.drawSmallTextBold("BRVARIO E-PAPER", bounds_.x + 18, bounds_.y + 10, 3, AppConfig::kBlack);
  display.drawLine(bounds_.x, bounds_.y + bounds_.height - 1, bounds_.x + bounds_.width - 1, bounds_.y + bounds_.height - 1, AppConfig::kBlack);
}

void HeaderWidget::renderDynamic(EpdDisplay& display, const VarioData& data) {
  display.fillRect(dynamicBounds(), AppConfig::kWhite);

  char timeText[8];
  formatClock(timeText, sizeof(timeText), data.timeOfDaySeconds);
  const int32_t timeCenterX = bounds_.x + bounds_.width / 2;
  display.drawSmallTextBoldAligned(timeText, timeCenterX, bounds_.y + 10, 3, AppConfig::kBlack, EpdDisplay::Align::Center);

  const int32_t right = bounds_.x + bounds_.width;
  const int32_t iconY = bounds_.y + 9;
  const int32_t batteryX = right - 132;
  int32_t iconRight = batteryX - 12;
  if (data.bluetoothActive) {
    iconRight -= 44;
    drawBluetoothIcon(display, iconRight, iconY, data.bluetoothConnected);
    iconRight -= 8;
  }
  if (data.wifiEnabled) {
    iconRight -= 56;
    drawWifiIcon(display, iconRight, iconY);
    iconRight -= 8;
  }
  iconRight -= 116;
  drawGpsStatus(display, iconRight, iconY, data.gpsFix, data.satellites);
  drawBatteryIcon(display, batteryX, bounds_.y + 11, data.batteryPercent, data.batteryCharging);

  char percentText[8];
  snprintf(percentText, sizeof(percentText), "%u%%", data.batteryPercent);
  display.drawSmallTextBoldAligned(percentText, right - 18, bounds_.y + 11, 3, AppConfig::kBlack, EpdDisplay::Align::Right);
}

Rect_t HeaderWidget::dynamicBounds() const {
  return {bounds_.x + bounds_.width / 2 - 96, bounds_.y + 5, bounds_.width / 2 + 78, bounds_.height - 10};
}
