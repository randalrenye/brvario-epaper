#include "SpeedGaugeWidget.h"

#include <math.h>
#include <stdio.h>

#include "config/AppConfig.h"
#include "ui/PilotIcon.h"

namespace {

static constexpr float kDegToRad = 0.01745329252F;
static constexpr int32_t kSpeedBlockHeight = 104;
static constexpr int32_t kCompassRadius = 118;

bool primaryLayout(const Rect_t& bounds) {
  return bounds.width >= 340;
}

int32_t speedBlockHeight(const Rect_t& bounds) {
  return primaryLayout(bounds) ? 76 : kSpeedBlockHeight;
}

int32_t compassRadiusFor(const Rect_t& bounds) {
  if (!primaryLayout(bounds)) {
    return kCompassRadius;
  }

  const int32_t topBlock = speedBlockHeight(bounds);
  const int32_t middleH = bounds.height - topBlock * 2;
  int32_t radiusByWidth = bounds.width / 2 - 48;
  int32_t radiusByHeight = middleH / 2 - 18;
  int32_t radius = radiusByWidth < radiusByHeight ? radiusByWidth : radiusByHeight;
  if (radius < 82) radius = 82;
  return radius;
}

int32_t compassOnlyRadiusFor(const Rect_t& bounds) {
  int32_t radiusByWidth = bounds.width / 2 - 22;
  int32_t radiusByHeight = bounds.height / 2 - 16;
  int32_t radius = radiusByWidth < radiusByHeight ? radiusByWidth : radiusByHeight;
  if (radius > 170) radius = 170;
  if (radius < 104) radius = 104;
  return radius;
}

float normalizeDeg(float value) {
  while (value < 0.0F) value += 360.0F;
  while (value >= 360.0F) value -= 360.0F;
  return value;
}

int32_t compassX(int32_t cx, float relativeDeg, int32_t radius) {
  return cx + static_cast<int32_t>(sinf(relativeDeg * kDegToRad) * radius);
}

int32_t compassY(int32_t cy, float relativeDeg, int32_t radius) {
  return cy - static_cast<int32_t>(cosf(relativeDeg * kDegToRad) * radius);
}

void drawThickLine(EpdDisplay& display, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color) {
  display.drawLine(x0, y0, x1, y1, color);
  display.drawLine(x0 + 1, y0, x1 + 1, y1, color);
  display.drawLine(x0 - 1, y0, x1 - 1, y1, color);
  display.drawLine(x0, y0 + 1, x1, y1 + 1, color);
}

int32_t vectorX(float deg, int32_t length) {
  return static_cast<int32_t>(sinf(deg * kDegToRad) * length);
}

int32_t vectorY(float deg, int32_t length) {
  return -static_cast<int32_t>(cosf(deg * kDegToRad) * length);
}

void drawWindPointer(EpdDisplay& display, int32_t cx, int32_t cy, float relativeDeg, int32_t length) {
  const int32_t tipX = compassX(cx, relativeDeg, 44);
  const int32_t tipY = compassY(cy, relativeDeg, 44);
  const int32_t tailX = compassX(cx, relativeDeg + 180.0F, length);
  const int32_t tailY = compassY(cy, relativeDeg + 180.0F, length);
  const int32_t baseX = compassX(cx, relativeDeg, 19);
  const int32_t baseY = compassY(cy, relativeDeg, 19);
  const int32_t wingX = vectorX(relativeDeg - 90.0F, 10);
  const int32_t wingY = vectorY(relativeDeg - 90.0F, 10);

  drawThickLine(display, tailX, tailY, baseX, baseY, AppConfig::kBlack);
  display.fillTriangle(tipX, tipY, baseX + wingX, baseY + wingY, baseX - wingX, baseY - wingY, AppConfig::kBlack);

  const int32_t fin1X = compassX(cx, relativeDeg + 180.0F, length - 18);
  const int32_t fin1Y = compassY(cy, relativeDeg + 180.0F, length - 18);
  const int32_t fin2X = compassX(cx, relativeDeg + 180.0F, length - 3);
  const int32_t fin2Y = compassY(cy, relativeDeg + 180.0F, length - 3);
  const int32_t tailWingX = vectorX(relativeDeg - 90.0F, 16);
  const int32_t tailWingY = vectorY(relativeDeg - 90.0F, 16);

  drawThickLine(display, fin1X, fin1Y, fin2X + tailWingX, fin2Y + tailWingY, AppConfig::kBlack);
  drawThickLine(display, fin1X, fin1Y, fin2X - tailWingX, fin2Y - tailWingY, AppConfig::kBlack);
  display.fillCircle(cx, cy, 5, AppConfig::kWhite);
  display.drawCircle(cx, cy, 6, AppConfig::kBlack);
}

void formatSpeed(char* text, size_t size, float value) {
  const int rounded = static_cast<int>(value + 0.5F);
  snprintf(text, size, "%d", rounded);
}

void drawSpeedValue(EpdDisplay& display, const char* text, int32_t x, int32_t y, uint8_t scale) {
  display.drawSmallTextBoldAligned(text, x, y, scale, AppConfig::kBlack, EpdDisplay::Align::Center);
  display.drawSmallTextBoldAligned(text, x + 2, y, scale, AppConfig::kBlack, EpdDisplay::Align::Center);
}

}  // namespace

void SpeedGaugeWidget::render(EpdDisplay& display, const VarioData& data) {
  renderStatic(display);
  renderDynamic(display, data);
}

void SpeedGaugeWidget::renderStatic(EpdDisplay& display) {
  display.fillRect(bounds_, AppConfig::kWhite);
  display.drawLine(bounds_.x, bounds_.y, bounds_.x, bounds_.y + bounds_.height - 1, AppConfig::kBlack);

  if (mode_ == Mode::CompassOnly) {
    display.drawSmallTextBoldAligned("RUMO / DIR. VENTO", bounds_.x + bounds_.width / 2, bounds_.y + 8, 2, AppConfig::kBlack,
                                     EpdDisplay::Align::Center);
    return;
  }

  const int32_t left = bounds_.x + 22;
  const int32_t right = bounds_.x + bounds_.width - 22;
  const int32_t topBlock = speedBlockHeight(bounds_);
  const int32_t speedBottom = bounds_.y + topBlock;
  const int32_t windTop = bounds_.y + bounds_.height - topBlock;
  const bool primary = primaryLayout(bounds_);

  display.drawLine(left, speedBottom, right, speedBottom, AppConfig::kBlack);
  display.drawLine(left, windTop, right, windTop, AppConfig::kBlack);

  display.drawSmallTextBoldAligned("VELOCIDADE SOLO", bounds_.x + bounds_.width / 2, bounds_.y + (primary ? 8 : 12), 2, AppConfig::kBlack,
                                   EpdDisplay::Align::Center);
  display.drawSmallTextBoldAligned("KM/H", right - 2, speedBottom - (primary ? 20 : 24), 2, AppConfig::kBlack, EpdDisplay::Align::Right);
  display.drawSmallTextBoldAligned("RUMO / DIR. VENTO", bounds_.x + bounds_.width / 2, speedBottom + (primary ? 2 : 6), 2,
                                   AppConfig::kBlack, EpdDisplay::Align::Center);
  display.drawSmallTextBoldAligned("VELOCIDADE VENTO", bounds_.x + bounds_.width / 2, windTop + (primary ? 8 : 12), 2, AppConfig::kBlack,
                                   EpdDisplay::Align::Center);
  display.drawSmallTextBoldAligned("KM/H", right - 2, bounds_.y + bounds_.height - (primary ? 24 : 28), 2, AppConfig::kBlack,
                                   EpdDisplay::Align::Right);
}

void SpeedGaugeWidget::renderDynamic(EpdDisplay& display, const VarioData& data) {
  const int32_t cx = bounds_.x + bounds_.width / 2;
  const bool compassOnly = mode_ == Mode::CompassOnly;
  const int32_t topBlock = speedBlockHeight(bounds_);
  const int32_t speedBottom = bounds_.y + topBlock;
  const int32_t windTop = bounds_.y + bounds_.height - topBlock;
  const bool primary = primaryLayout(bounds_);
  const int32_t cy = compassOnly ? bounds_.y + bounds_.height / 2 + 10 : speedBottom + (windTop - speedBottom) / 2 + (primary ? 6 : 8);
  const int32_t r = compassOnly ? compassOnlyRadiusFor(bounds_) : compassRadiusFor(bounds_);

  display.drawCircle(cx, cy, r, AppConfig::kBlack);
  display.drawCircle(cx, cy, r - 1, AppConfig::kBlack);

  for (int bearing = 0; bearing < 360; bearing += 30) {
    const float rel = normalizeDeg(static_cast<float>(bearing) - data.courseDeg);
    const bool cardinal = bearing % 90 == 0;
    const int32_t tickOuterX = compassX(cx, rel, r);
    const int32_t tickOuterY = compassY(cy, rel, r);
    const int32_t tickInnerX = compassX(cx, rel, r - (cardinal ? 16 : 10));
    const int32_t tickInnerY = compassY(cy, rel, r - (cardinal ? 16 : 10));
    if (cardinal) {
      drawThickLine(display, tickInnerX, tickInnerY, tickOuterX, tickOuterY, AppConfig::kBlack);
    } else {
      display.drawLine(tickInnerX, tickInnerY, tickOuterX, tickOuterY, AppConfig::kBlack);
    }
  }

  struct CardinalLabel {
    float bearing;
    const char* label;
  };
  const CardinalLabel labels[] = {
      {0.0F, "N"},
      {90.0F, "L"},
      {180.0F, "S"},
      {270.0F, "O"},
  };
  for (const CardinalLabel& item : labels) {
    const float rel = normalizeDeg(item.bearing - data.courseDeg);
    const int32_t labelRadius = r - (primary ? 38 : 42);
    const int32_t labelX = compassX(cx, rel, labelRadius);
    const int32_t labelY = compassY(cy, rel, labelRadius) - 11;
    display.fillRect({labelX - 15, labelY - 3, 30, 28}, AppConfig::kWhite);
    display.drawSmallTextBoldAligned(item.label, labelX, labelY, 3, AppConfig::kBlack, EpdDisplay::Align::Center);
  }

  PilotIcon::draw(display, cx, cy - r + (primary ? 15 : 16), 0.0F, primary ? 15 : 16, primary ? 10 : 11, primary ? 9 : 10,
                  AppConfig::kBlack);

  if (data.windQuality != WindQuality::None && data.windSpeedKmh > 0.5F) {
    const float windRelative = normalizeDeg(data.windDirectionDeg - data.courseDeg);
    drawWindPointer(display, cx, cy, windRelative, r - (primary ? 56 : 60));
  }

  if (compassOnly) {
    return;
  }

  char text[12];
  const int32_t valueX = cx - (primary ? 8 : 16);
  const uint8_t valueScale = primary ? 6 : 8;
  formatSpeed(text, sizeof(text), data.groundSpeedKmh);
  drawSpeedValue(display, text, valueX, bounds_.y + (primary ? 31 : 42), valueScale);

  if (data.windQuality != WindQuality::None) {
    formatSpeed(text, sizeof(text), data.windSpeedKmh);
    drawSpeedValue(display, text, valueX, windTop + (primary ? 31 : 42), valueScale);
  }
}

Rect_t SpeedGaugeWidget::dynamicBounds() const {
  if (mode_ == Mode::CompassOnly) {
    return {bounds_.x + 10, bounds_.y + 28, bounds_.width - 20, bounds_.height - 34};
  }
  return {bounds_.x + 6, bounds_.y + 2, bounds_.width - 12, bounds_.height - 6};
}

Rect_t SpeedGaugeWidget::compassBounds() const {
  if (mode_ == Mode::CompassOnly) {
    return dynamicBounds();
  }
  const int32_t topBlock = speedBlockHeight(bounds_);
  const int32_t speedBottom = bounds_.y + topBlock;
  const int32_t windTop = bounds_.y + bounds_.height - topBlock;
  return {bounds_.x + 10, speedBottom + 26, bounds_.width - 20, windTop - speedBottom - 30};
}

Rect_t SpeedGaugeWidget::groundSpeedValueBounds() const {
  if (mode_ == Mode::CompassOnly) {
    return dynamicBounds();
  }
  return {bounds_.x + 12, bounds_.y + (primaryLayout(bounds_) ? 26 : 34), bounds_.width - 24, primaryLayout(bounds_) ? 52 : 64};
}

Rect_t SpeedGaugeWidget::windSpeedValueBounds() const {
  if (mode_ == Mode::CompassOnly) {
    return dynamicBounds();
  }
  const int32_t windTop = bounds_.y + bounds_.height - speedBlockHeight(bounds_);
  return {bounds_.x + 12, windTop + (primaryLayout(bounds_) ? 26 : 34), bounds_.width - 24, primaryLayout(bounds_) ? 52 : 64};
}
