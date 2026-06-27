#include "VarioGaugeWidget.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "config/AppConfig.h"

namespace {

float clampFloat(float value, float lo, float hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

void drawThickLine(EpdDisplay& display, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color, uint8_t thickness) {
  display.drawLine(x0, y0, x1, y1, color);
  if (thickness >= 2) {
    display.drawLine(x0 + 1, y0, x1 + 1, y1, color);
  }
  if (thickness >= 3) {
    display.drawLine(x0 - 1, y0, x1 - 1, y1, color);
  }
  if (thickness >= 4) {
    display.drawLine(x0, y0 + 1, x1, y1 + 1, color);
  }
  if (thickness >= 5) {
    display.drawLine(x0, y0 - 1, x1, y1 - 1, color);
  }
}

void drawHeavySmallText(EpdDisplay& display, const char* text, int32_t x, int32_t y, uint8_t scale, uint8_t color) {
  display.drawSmallTextBoldAligned(text, x, y, scale, color, EpdDisplay::Align::Center);
  display.drawSmallTextBoldAligned(text, x + 2, y, scale, color, EpdDisplay::Align::Center);
}

void drawMetricText(EpdDisplay& display, const char* text, int32_t x, int32_t y, uint8_t scale) {
  display.drawSmallTextBold(text, x, y, scale, AppConfig::kBlack);
}

void drawMetricValue(EpdDisplay& display, const char* text, int32_t x, int32_t y, uint8_t scale) {
  display.drawSmallTextBoldAligned(text, x, y, scale, AppConfig::kBlack, EpdDisplay::Align::Right);
}

void drawSideInfoTitle(EpdDisplay& display, const Rect_t& block, const char* label, const char* unit) {
  display.drawSmallTextBold(label, block.x + 2, block.y + 12, 2, AppConfig::kBlack);
  display.drawSmallTextBoldAligned(unit, block.x + block.width - 2, block.y + block.height - 22, 2, AppConfig::kBlack,
                                   EpdDisplay::Align::Right);
}

uint8_t sideInfoValueScale(const char* text) {
  const size_t length = strlen(text);
  if (length <= 4) return 8;
  if (length <= 5) return 7;
  return 6;
}

void drawSideInfoValue(EpdDisplay& display, const Rect_t& block, const char* text) {
  const uint8_t scale = sideInfoValueScale(text);
  const int32_t valueX = block.x + block.width / 2 - 18;
  const int32_t valueY = block.y + 42;
  display.drawSmallTextBoldAligned(text, valueX, valueY, scale, AppConfig::kBlack, EpdDisplay::Align::Center);
  display.drawSmallTextBoldAligned(text, valueX + 2, valueY, scale, AppConfig::kBlack, EpdDisplay::Align::Center);
}

bool showThermalCoreMetric(const VarioData& data) {
  return data.thermalCoreConfidencePercent >= 25 && data.varioMs > 0.10F;
}

void formatDuration(char* text, size_t size, uint32_t seconds) {
  const uint32_t hours = seconds / 3600UL;
  const uint32_t minutes = (seconds / 60UL) % 60UL;
  const uint32_t secs = seconds % 60UL;
  snprintf(text, size, "%lu:%02lu:%02lu",
           static_cast<unsigned long>(hours),
           static_cast<unsigned long>(minutes),
           static_cast<unsigned long>(secs));
}

void drawArc(EpdDisplay& display, int32_t cx, int32_t cy, int32_t radius, float startDeg, float endDeg, uint8_t color, uint8_t thickness) {
  int32_t lastX = cx + static_cast<int32_t>(cosf(startDeg * 0.01745329252F) * radius);
  int32_t lastY = cy + static_cast<int32_t>(sinf(startDeg * 0.01745329252F) * radius);
  for (float deg = startDeg + 4.0F; deg <= endDeg; deg += 4.0F) {
    const int32_t x = cx + static_cast<int32_t>(cosf(deg * 0.01745329252F) * radius);
    const int32_t y = cy + static_cast<int32_t>(sinf(deg * 0.01745329252F) * radius);
    drawThickLine(display, lastX, lastY, x, y, color, thickness);
    lastX = x;
    lastY = y;
  }
}

void drawNeedle(EpdDisplay& display, int32_t cx, int32_t cy, float angle, int32_t length) {
  const float ca = cosf(angle);
  const float sa = sinf(angle);
  const int32_t tipX = cx + static_cast<int32_t>(ca * length);
  const int32_t tipY = cy + static_cast<int32_t>(sa * length);
  const int32_t baseX = cx + static_cast<int32_t>(ca * 8);
  const int32_t baseY = cy + static_cast<int32_t>(sa * 8);
  const int32_t wingX = static_cast<int32_t>(cosf(angle - 1.57079632679F) * 13);
  const int32_t wingY = static_cast<int32_t>(sinf(angle - 1.57079632679F) * 13);

  display.fillTriangle(tipX, tipY, baseX + wingX, baseY + wingY, baseX - wingX, baseY - wingY, AppConfig::kBlack);
  display.fillCircle(cx, cy, 11, AppConfig::kWhite);
  display.drawCircle(cx, cy, 11, AppConfig::kBlack);
  display.drawCircle(cx, cy, 10, AppConfig::kBlack);
}

}  // namespace

void VarioGaugeWidget::render(EpdDisplay& display, const VarioData& data) {
  renderStatic(display);
  renderDynamic(display, data);
}

void VarioGaugeWidget::renderStatic(EpdDisplay& display) {
  display.fillRect(bounds_, AppConfig::kWhite);
  display.drawLine(bounds_.x + bounds_.width - 1, bounds_.y, bounds_.x + bounds_.width - 1, bounds_.y + bounds_.height - 1, AppConfig::kBlack);

  const int32_t cx = centerX();
  const int32_t cy = centerY();
  const int32_t r = radius();
  const int32_t innerCircleR = (r * 77) / 100;
  const bool compactMode = compact();
  const bool sideInfo = sideInfoActive();

  if (sideInfo) {
    const Rect_t top = topSideInfoBounds();
    const Rect_t bottom = bottomSideInfoBounds();
    display.drawLine(bounds_.x + 18, top.y + top.height, bounds_.x + bounds_.width - 18, top.y + top.height, AppConfig::kBlack);
    display.drawLine(bounds_.x + 18, bottom.y - 1, bounds_.x + bounds_.width - 18, bottom.y - 1, AppConfig::kBlack);
    if (sideInfoMode_ == SideInfoMode::Altitude) {
      drawSideInfoTitle(display, top, "ALT GPS", "m");
      drawSideInfoTitle(display, bottom, "ALT AGL", "m");
    } else {
      drawSideInfoTitle(display, top, "VELOCIDADE SOLO", "KM/H");
      drawSideInfoTitle(display, bottom, "VELOCIDADE VENTO", "KM/H");
    }
  }

  display.drawSmallTextBoldAligned("VARIO", cx, gaugeArea().y + 8, 2, AppConfig::kBlack, EpdDisplay::Align::Center);

  drawArc(display, cx, cy, innerCircleR, 158.0F, 382.0F, AppConfig::kBlack, 2);

  for (int tick = -40; tick <= 40; ++tick) {
    const float value = static_cast<float>(tick) / 10.0F;
    const float angle = valueToAngleRad(value);
    const float ca = cosf(angle);
    const float sa = sinf(angle);
    const bool major = tick % 10 == 0;
    const bool medium = tick % 5 == 0;
    const int32_t tickLength = major ? 31 : medium ? 20 : 12;
    const int32_t tickOuterX = cx + static_cast<int32_t>(ca * r);
    const int32_t tickOuterY = cy + static_cast<int32_t>(sa * r);
    const int32_t tickInnerX = cx + static_cast<int32_t>(ca * (r - tickLength));
    const int32_t tickInnerY = cy + static_cast<int32_t>(sa * (r - tickLength));
    drawThickLine(display, tickInnerX, tickInnerY, tickOuterX, tickOuterY, AppConfig::kBlack, major ? 4 : medium ? 2 : 1);
  }

  for (int value = -4; value <= 4; ++value) {
    const float angle = valueToAngleRad(static_cast<float>(value));
    const float ca = cosf(angle);
    const float sa = sinf(angle);
    char label[5];
    if (value > 0) {
      snprintf(label, sizeof(label), "+%d", value);
    } else {
      snprintf(label, sizeof(label), "%d", value);
    }
    const int32_t absValue = value < 0 ? -value : value;
    const int32_t edgeOffset = absValue >= 4 ? (compactMode ? 10 : 8) : absValue == 3 ? (compactMode ? 5 : 4) : 0;
    const int32_t labelRadius = r + (compactMode ? 8 : 22) + edgeOffset;
    const int32_t labelX = cx + static_cast<int32_t>(ca * labelRadius);
    const int32_t labelY = cy + static_cast<int32_t>(sa * labelRadius) - 14;
    display.drawSmallTextBoldAligned(label, labelX, labelY, compactMode ? 2 : 3, AppConfig::kBlack, EpdDisplay::Align::Center);
  }

  const int32_t metricY = bounds_.y + bounds_.height - (compactMode ? 96 : 62);
  const uint8_t metricScale = 2;
  if (sideInfo) {
    return;
  } else if (compactMode) {
    return;
  } else {
    display.drawLine(bounds_.x + 20, metricY - 1, bounds_.x + bounds_.width - 20, metricY - 1, AppConfig::kBlack);
    display.drawLine(bounds_.x + 20, metricY + 31, bounds_.x + bounds_.width - 20, metricY + 31, AppConfig::kBlack);
  }
}

void VarioGaugeWidget::renderDynamic(EpdDisplay& display, const VarioData& data) {
  const int32_t cx = centerX();
  const int32_t cy = centerY();
  const int32_t r = radius();
  const bool compactMode = compact();
  const bool sideInfo = sideInfoActive();

  if (sideInfo) {
    char auxText[12];
    if (sideInfoMode_ == SideInfoMode::Altitude) {
      snprintf(auxText, sizeof(auxText), "%d", static_cast<int>(data.altitudeGpsM + 0.5F));
      drawSideInfoValue(display, topSideInfoBounds(), auxText);
      snprintf(auxText, sizeof(auxText), "%d", static_cast<int>(data.altitudeAglM + 0.5F));
      drawSideInfoValue(display, bottomSideInfoBounds(), auxText);
    } else {
      snprintf(auxText, sizeof(auxText), "%d", static_cast<int>(data.groundSpeedKmh + 0.5F));
      drawSideInfoValue(display, topSideInfoBounds(), auxText);
      if (data.windQuality != WindQuality::None) {
        snprintf(auxText, sizeof(auxText), "%d", static_cast<int>(data.windSpeedKmh + 0.5F));
      } else {
        auxText[0] = '\0';
      }
      drawSideInfoValue(display, bottomSideInfoBounds(), auxText);
    }
  }

  const float clamped = clampFloat(data.varioMs, -4.0F, 4.0F);
  const float angle = valueToAngleRad(clamped);
  drawNeedle(display, cx, cy, angle, r - (compactMode ? 28 : 40));

  char varioText[12];
  snprintf(varioText, sizeof(varioText), "%+.1f", data.varioMs);
  drawHeavySmallText(display, varioText, cx - (sideInfo ? 22 : compactMode ? 18 : 30),
                     cy + (sideInfo ? 42 : compactMode ? 32 : 47), sideInfo ? 6 : compactMode ? 6 : 9, AppConfig::kBlack);
  display.drawSmallTextBold("m/s", cx + (sideInfo ? 70 : compactMode ? 62 : 106), cy + (sideInfo ? 66 : compactMode ? 55 : 83), 2,
                            AppConfig::kBlack);

  const int32_t metricY = bounds_.y + bounds_.height - (compactMode ? 96 : 62);
  const uint8_t metricScale = 2;
  if (sideInfo) {
    return;
  }

  const bool thermalCoreMetric = showThermalCoreMetric(data);
  char coreMetricText[12];
  snprintf(coreMetricText, sizeof(coreMetricText), "%u%%", static_cast<unsigned>(data.thermalCoreConfidencePercent));
  char gainMetricText[12];
  snprintf(gainMetricText, sizeof(gainMetricText), "%.0f m", data.ganhoTermicaM);
  char glideMetricText[12];
  snprintf(glideMetricText, sizeof(glideMetricText), "%.1f", data.glideRatio);

  if (compactMode) {
    display.fillRect({bounds_.x + 10, metricY, bounds_.width - 20, 56}, AppConfig::kWhite);
    if (thermalCoreMetric) {
      drawMetricText(display, "NUCLEO", bounds_.x + 14, metricY + 5, metricScale);
      drawMetricValue(display, coreMetricText, bounds_.x + bounds_.width - 12, metricY + 5, metricScale);
      drawMetricText(display, "GANHO", bounds_.x + 14, metricY + 31, metricScale);
      drawMetricValue(display, gainMetricText, bounds_.x + bounds_.width - 12, metricY + 31, metricScale);
    } else {
      drawMetricText(display, "PLANEIO", cx - 78, metricY + 18, metricScale);
      drawMetricValue(display, glideMetricText, cx + 86, metricY + 18, metricScale);
    }
  } else {
    display.fillRect({bounds_.x + 20, metricY, bounds_.width - 40, 31}, AppConfig::kWhite);
    if (thermalCoreMetric) {
      drawThickLine(display, cx, metricY - 1, cx, metricY + 28, AppConfig::kBlack, 2);
      drawMetricText(display, "NUCLEO", cx - 176, metricY + 5, metricScale);
      drawMetricValue(display, coreMetricText, cx - 34, metricY + 5, metricScale);
      drawMetricText(display, "GANHO", cx + 18, metricY + 5, metricScale);
      drawMetricValue(display, gainMetricText, cx + 178, metricY + 5, metricScale);
    } else {
      drawMetricText(display, "PLANEIO", cx - 100, metricY + 8, 2);
      drawMetricValue(display, glideMetricText, cx + 100, metricY + 8, 2);
    }
  }

  char duration[16];
  formatDuration(duration, sizeof(duration), data.elapsedSeconds);
  char flightTime[32];
  snprintf(flightTime, sizeof(flightTime), "DURAÇAO VOO %s", duration);
  if (!sideInfo) {
    display.drawSmallTextBoldAligned(flightTime, cx, bounds_.y + bounds_.height - (compactMode ? 34 : 24), 2, AppConfig::kBlack,
                                     EpdDisplay::Align::Center);
  }
}

float VarioGaugeWidget::valueToAngleRad(float value) const {
  const float clamped = clampFloat(value, -4.0F, 4.0F);
  const float degrees = 160.0F + ((clamped + 4.0F) / 8.0F) * 220.0F;
  return degrees * 0.01745329252F;
}

Rect_t VarioGaugeWidget::dynamicBounds() const {
  if (sideInfoActive()) {
    return {bounds_.x + 8, bounds_.y + 2, bounds_.width - 16, bounds_.height - 6};
  }
  return {bounds_.x + 20, bounds_.y + 38, bounds_.width - 40, bounds_.height - 30};
}

Rect_t VarioGaugeWidget::needleBounds() const {
  const int32_t r = radius() + 42;
  return {centerX() - r, centerY() - r, r * 2, r * 2};
}

Rect_t VarioGaugeWidget::valueBounds() const {
  return {bounds_.x + 18, bounds_.y + 6, bounds_.width - 36, bounds_.height - 14};
}

Rect_t VarioGaugeWidget::gaugeArea() const {
  if (sideInfoActive()) {
    return {bounds_.x, bounds_.y + 104, bounds_.width, bounds_.height - 208};
  }
  return bounds_;
}

Rect_t VarioGaugeWidget::topSideInfoBounds() const {
  return {bounds_.x + 10, bounds_.y, bounds_.width - 20, 104};
}

Rect_t VarioGaugeWidget::bottomSideInfoBounds() const {
  return {bounds_.x + 10, bounds_.y + bounds_.height - 104, bounds_.width - 20, 104};
}

int32_t VarioGaugeWidget::centerX() const {
  const Rect_t area = gaugeArea();
  return area.x + area.width / 2;
}

int32_t VarioGaugeWidget::centerY() const {
  const Rect_t area = gaugeArea();
  return area.y + area.height / 2 + (sideInfoActive() ? 28 : 12);
}

int32_t VarioGaugeWidget::radius() const {
  const Rect_t area = gaugeArea();
  int32_t maxRadiusW = (area.width - 70) / 2;
  int32_t maxRadiusH = (area.height - (sideInfoActive() ? 74 : 142)) / 2;
  int32_t r = maxRadiusW < maxRadiusH ? maxRadiusW : maxRadiusH;
  if (r > 150) r = 150;
  if (r < (sideInfoActive() ? 72 : 82)) r = sideInfoActive() ? 72 : 82;
  return r;
}

bool VarioGaugeWidget::compact() const {
  return bounds_.width < 340;
}

bool VarioGaugeWidget::sideInfoActive() const {
  return compact() && sideInfoMode_ != SideInfoMode::None;
}
