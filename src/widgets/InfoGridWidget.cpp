#include "InfoGridWidget.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "config/AppConfig.h"
#include "ui/PilotIcon.h"

namespace {

static constexpr float kDegToRad = 0.01745329252F;

uint8_t altitudeValueScale(const char* value) {
  const size_t length = strlen(value);
  if (length <= 4) return 8;
  if (length <= 5) return 7;
  return 6;
}

bool primaryLayout(const Rect_t& bounds) {
  return bounds.width >= 340;
}

int32_t altitudeBlockHeight(const Rect_t& bounds) {
  return primaryLayout(bounds) ? 70 : 104;
}

uint8_t altitudeScaleForBlock(const Rect_t& block, const char* value) {
  uint8_t scale = altitudeValueScale(value);
  if (block.height < 90 && scale > 5) {
    scale = 5;
  }
  return scale;
}

float normalizeDeg(float value) {
  while (value < 0.0F) value += 360.0F;
  while (value >= 360.0F) value -= 360.0F;
  return value;
}

float clampFloat(float value, float lo, float hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

int32_t radarX(int32_t cx, float bearingDeg, int32_t radius) {
  return cx + static_cast<int32_t>(sinf(bearingDeg * kDegToRad) * radius);
}

int32_t radarY(int32_t cy, float bearingDeg, int32_t radius) {
  return cy - static_cast<int32_t>(cosf(bearingDeg * kDegToRad) * radius);
}

void drawBoldLine(EpdDisplay& display, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
  display.drawLine(x0, y0, x1, y1, AppConfig::kBlack);
  display.drawLine(x0 + 1, y0, x1 + 1, y1, AppConfig::kBlack);
  display.drawLine(x0, y0 + 1, x1, y1 + 1, AppConfig::kBlack);
}

void drawBoldCircle(EpdDisplay& display, int32_t cx, int32_t cy, int32_t radius) {
  display.drawCircle(cx, cy, radius, AppConfig::kBlack);
  display.drawCircle(cx, cy, radius - 1, AppConfig::kBlack);
}

void drawDriftArrow(EpdDisplay& display, int32_t cx, int32_t cy, float bearingDeg, int32_t length) {
  const int32_t startX = radarX(cx, bearingDeg, 16);
  const int32_t startY = radarY(cy, bearingDeg, 16);
  const int32_t tipX = radarX(cx, bearingDeg, length);
  const int32_t tipY = radarY(cy, bearingDeg, length);
  const int32_t baseX = radarX(cx, bearingDeg, length - 14);
  const int32_t baseY = radarY(cy, bearingDeg, length - 14);
  const int32_t wingX = static_cast<int32_t>(sinf((bearingDeg - 90.0F) * kDegToRad) * 10);
  const int32_t wingY = -static_cast<int32_t>(cosf((bearingDeg - 90.0F) * kDegToRad) * 10);

  drawBoldLine(display, startX, startY, baseX, baseY);
  display.fillCircle(startX, startY, 3, AppConfig::kBlack);
  display.fillTriangle(tipX, tipY, baseX + wingX, baseY + wingY, baseX - wingX, baseY - wingY, AppConfig::kBlack);
}

void drawPilotMarker(EpdDisplay& display, int32_t cx, int32_t cy, float bearingDeg) {
  PilotIcon::draw(display, cx, cy, bearingDeg, 19, 12, 13, AppConfig::kBlack);
}

uint8_t thermalPointRadius(float liftMs, bool largeLayout) {
  const float lift = clampFloat(liftMs, 0.10F, 5.0F);
  const float strongStartRadius = largeLayout ? 10.0F : 8.0F;
  const float maxRadius = largeLayout ? 16.0F : 12.0F;
  float radius = 3.0F;
  if (lift <= 2.5F) {
    radius += (lift - 0.10F) * (strongStartRadius - 3.0F) / 2.40F;
  } else {
    radius = strongStartRadius + (lift - 2.5F) * (maxRadius - strongStartRadius) / 2.5F;
  }
  return static_cast<uint8_t>(radius + 0.5F);
}

uint8_t thermalPointFill(float liftMs) {
  if (liftMs >= 2.0F) return AppConfig::kBlack;
  if (liftMs >= 1.0F) return AppConfig::kMid;
  return AppConfig::kLight;
}

uint8_t thermalPointOutline(float liftMs) {
  if (liftMs >= 1.0F) return AppConfig::kBlack;
  return AppConfig::kMid;
}

void drawRangeLabel(EpdDisplay& display, int32_t x, int32_t y, float meters, uint8_t scale) {
  char text[10];
  snprintf(text, sizeof(text), "%dm", static_cast<int>(meters + 0.5F));
  const int32_t boxW = scale > 1 ? 54 : 36;
  const int32_t boxH = scale > 1 ? 20 : 14;
  display.fillRect({x - boxW / 2, y - 3, boxW, boxH}, AppConfig::kWhite);
  if (scale > 1) {
    display.drawSmallTextAligned(text, x, y, scale, AppConfig::kBlack, EpdDisplay::Align::Center);
  } else {
    display.drawSmallTextBoldAligned(text, x, y, scale, AppConfig::kBlack, EpdDisplay::Align::Center);
  }
}

void drawThermalRangeLabels(EpdDisplay& display, int32_t cx, int32_t cy, int32_t r, float rangeM, uint8_t scale) {
  const int32_t innerR = r / 2;
  const int32_t midR = (r * 3) / 4;
  const int32_t outerR = r - (scale > 1 ? 14 : 8);

  drawRangeLabel(display, radarX(cx, 135.0F, innerR), radarY(cy, 135.0F, innerR), rangeM * 0.50F, scale);
  drawRangeLabel(display, radarX(cx, 135.0F, midR), radarY(cy, 135.0F, midR), rangeM * 0.75F, scale);
  drawRangeLabel(display, radarX(cx, 135.0F, outerR), radarY(cy, 135.0F, outerR), rangeM, scale);
}

}  // namespace

void InfoGridWidget::render(EpdDisplay& display, const VarioData& data) {
  renderStatic(display);
  renderDynamic(display, data);
}

void InfoGridWidget::renderStatic(EpdDisplay& display) {
  display.fillRect(bounds_, AppConfig::kWhite);

  if (mode_ == Mode::ThermalOnly) {
    drawThermalStatic(display, bounds_);
    return;
  }

  for (uint8_t i = 0; i < cellCount(); ++i) {
    const Rect_t block = blockBounds(i);
    if (i > 0) {
      display.drawLine(bounds_.x + 18, block.y, bounds_.x + bounds_.width - 18, block.y, AppConfig::kBlack);
    }
  }

  drawBlockStatic(display, blockBounds(0), "ALT GPS");
  drawThermalStatic(display, blockBounds(1));
  drawBlockStatic(display, blockBounds(2), "ALT AGL");
}

void InfoGridWidget::renderDynamic(EpdDisplay& display, const VarioData& data) {
  if (mode_ == Mode::ThermalOnly) {
    drawThermalDynamic(display, bounds_, data);
    return;
  }

  char value[16];

  snprintf(value, sizeof(value), "%d", static_cast<int>(data.altitudeGpsM + 0.5F));
  drawBlockValue(display, blockBounds(0), value);

  drawThermalDynamic(display, blockBounds(1), data);

  snprintf(value, sizeof(value), "%d", static_cast<int>(data.altitudeAglM + 0.5F));
  drawBlockValue(display, blockBounds(2), value);
}

Rect_t InfoGridWidget::blockBounds(uint8_t index) const {
  const int32_t altitudeBlockH = altitudeBlockHeight(bounds_);
  int32_t y = bounds_.y;
  int32_t h = altitudeBlockH;
  if (index == 1) {
    y = bounds_.y + altitudeBlockH;
    h = bounds_.height - altitudeBlockH * 2;
  } else if (index == 2) {
    y = bounds_.y + bounds_.height - altitudeBlockH;
  }
  return {bounds_.x + 10, y, bounds_.width - 20, h};
}

void InfoGridWidget::drawBlockStatic(EpdDisplay& display, const Rect_t& block, const char* label) {
  display.drawSmallTextBold(label, block.x + 2, block.y + 12, 2, AppConfig::kBlack);
  display.drawSmallTextBoldAligned("m", block.x + block.width - 4, block.y + block.height - 22, 2, AppConfig::kBlack, EpdDisplay::Align::Right);
}

void InfoGridWidget::drawBlockValue(EpdDisplay& display, const Rect_t& block, const char* value) {
  const uint8_t scale = altitudeScaleForBlock(block, value);
  const int32_t valueX = block.x + block.width / 2 - 18;
  const int32_t valueY = block.y + (block.height < 90 ? 30 : 42);
  display.drawSmallTextBoldAligned(value, valueX, valueY, scale, AppConfig::kBlack, EpdDisplay::Align::Center);
  display.drawSmallTextBoldAligned(value, valueX + 2, valueY, scale, AppConfig::kBlack, EpdDisplay::Align::Center);
}

void InfoGridWidget::drawThermalStatic(EpdDisplay& display, const Rect_t& block) {
  const bool primary = primaryLayout(bounds_);
  const int32_t cx = block.x + block.width / 2;
  const int32_t maxWidthR = block.width / 2 - (mode_ == Mode::ThermalOnly ? 15 : 5);
  const int32_t maxHeightR = block.height / 2 - (primary ? (mode_ == Mode::ThermalOnly ? 16 : 10) : 19);
  const int32_t r = maxWidthR < maxHeightR ? maxWidthR : maxHeightR;
  const int32_t cy = block.y + block.height / 2 + (primary ? 10 : 8);

  if (primary) {
    display.drawSmallTextBoldAligned("ASSISTENTE DE TERMICA", block.x + block.width / 2, block.y + 6, 2, AppConfig::kBlack,
                                     EpdDisplay::Align::Center);
  } else {
    display.drawSmallTextBoldAligned("ASSISTENTE TERMICA", block.x + block.width / 2, block.y + 6, 2, AppConfig::kBlack,
                                     EpdDisplay::Align::Center);
  }
  drawBoldCircle(display, cx, cy, r);
  drawBoldCircle(display, cx, cy, r / 2);
  drawBoldCircle(display, cx, cy, (r * 3) / 4);
  const int32_t axisInset = 24;
  drawBoldLine(display, cx - r + axisInset, cy, cx + r - axisInset, cy);
  drawBoldLine(display, cx, cy - r + axisInset, cx, cy + r - axisInset);
  display.drawSmallTextBoldAligned("N", cx, cy - r + 3, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  display.drawSmallTextBoldAligned("S", cx, cy + r - 17, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  display.drawSmallTextBoldAligned("L", cx + r - 9, cy - 7, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  display.drawSmallTextBoldAligned("O", cx - r + 9, cy - 7, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
}

void InfoGridWidget::drawThermalDynamic(EpdDisplay& display, const Rect_t& block, const VarioData& data) {
  const bool primary = primaryLayout(bounds_);
  const int32_t cx = block.x + block.width / 2;
  const int32_t maxWidthR = block.width / 2 - (mode_ == Mode::ThermalOnly ? 15 : 5);
  const int32_t maxHeightR = block.height / 2 - (primary ? (mode_ == Mode::ThermalOnly ? 16 : 10) : 19);
  const int32_t r = maxWidthR < maxHeightR ? maxWidthR : maxHeightR;
  const int32_t cy = block.y + block.height / 2 + (primary ? 10 : 8);
  const bool largeBubbles = primary && mode_ == Mode::ThermalOnly;
  const int32_t maxBubbleRadius = largeBubbles ? 16 : 12;
  const int32_t maxPointR = r - maxBubbleRadius - 2;
  const float rangeM = data.thermalRangeM > 1.0F ? data.thermalRangeM : 120.0F;

  const float driftBearing = normalizeDeg(data.thermalDriftDeg);
  drawDriftArrow(display, cx, cy, driftBearing, (r * 68) / 100);

  const uint8_t pointCount = data.thermalPointCount < kThermalAssistPoints ? data.thermalPointCount : kThermalAssistPoints;
  int32_t drawnX[kThermalAssistPoints] = {};
  int32_t drawnY[kThermalAssistPoints] = {};
  uint8_t drawnRadius[kThermalAssistPoints] = {};
  uint8_t drawnCount = 0;
  for (int8_t strengthPass = 2; strengthPass >= 0; --strengthPass) {
    for (int16_t i = static_cast<int16_t>(pointCount) - 1; i >= 0; --i) {
      const ThermalAssistPoint& point = data.thermalPoints[i];
      const uint8_t pointPass = point.liftMs >= 2.5F ? 2 : point.liftMs >= 1.0F ? 1 : 0;
      if (pointPass != static_cast<uint8_t>(strengthPass)) continue;

      float px = (point.eastM / rangeM) * static_cast<float>(maxPointR);
      float py = (-point.northM / rangeM) * static_cast<float>(maxPointR);
      const float distance = sqrtf(px * px + py * py);
      if (distance > static_cast<float>(maxPointR) && distance > 0.0F) {
        const float scale = static_cast<float>(maxPointR) / distance;
        px *= scale;
        py *= scale;
      }

      const int32_t pointX = cx + static_cast<int32_t>(px);
      const int32_t pointY = cy + static_cast<int32_t>(py);
      const uint8_t radius = thermalPointRadius(point.liftMs, largeBubbles);
      bool overlaps = false;
      for (uint8_t drawn = 0; drawn < drawnCount; ++drawn) {
        const int32_t dx = pointX - drawnX[drawn];
        const int32_t dy = pointY - drawnY[drawn];
        const int32_t minDistance = static_cast<int32_t>(radius) + drawnRadius[drawn] - 2;
        if (dx * dx + dy * dy < minDistance * minDistance) {
          overlaps = true;
          break;
        }
      }
      if (overlaps) continue;

      display.fillCircle(pointX, pointY, radius, thermalPointFill(point.liftMs));
      display.drawCircle(pointX, pointY, radius, thermalPointOutline(point.liftMs));
      if (point.liftMs >= 2.0F) {
        display.drawCircle(pointX, pointY, radius - 1, AppConfig::kBlack);
      }
      drawnX[drawnCount] = pointX;
      drawnY[drawnCount] = pointY;
      drawnRadius[drawnCount] = radius;
      ++drawnCount;
    }
  }

  float pilotPx = (data.thermalPilotEastM / rangeM) * static_cast<float>(maxPointR);
  float pilotPy = (-data.thermalPilotNorthM / rangeM) * static_cast<float>(maxPointR);
  const float pilotDistance = sqrtf(pilotPx * pilotPx + pilotPy * pilotPy);
  if (pilotDistance > static_cast<float>(maxPointR) && pilotDistance > 0.0F) {
    const float scale = static_cast<float>(maxPointR) / pilotDistance;
    pilotPx *= scale;
    pilotPy *= scale;
  }
  drawPilotMarker(display, cx + static_cast<int32_t>(pilotPx), cy + static_cast<int32_t>(pilotPy), normalizeDeg(data.courseDeg));

  drawThermalRangeLabels(display, cx, cy, r, rangeM, mode_ == Mode::ThermalOnly ? 2 : 1);
}

Rect_t InfoGridWidget::dynamicBounds() const {
  if (mode_ == Mode::ThermalOnly) {
    return {bounds_.x + 8, bounds_.y + 20, bounds_.width - 16, bounds_.height - 26};
  }
  return {bounds_.x + 8, bounds_.y + 34, bounds_.width - 16, bounds_.height - 42};
}

Rect_t InfoGridWidget::cellValueBounds(uint8_t index) const {
  if (mode_ == Mode::ThermalOnly) {
    return dynamicBounds();
  }
  const Rect_t block = blockBounds(index);
  if (index == 1) {
    return {block.x - 8, block.y + 20, block.width + 16, block.height - 20};
  }
  return {block.x - 8, block.y + 36, block.width + 16, block.height - 36};
}
