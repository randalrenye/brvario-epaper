#include "TrendWidget.h"

#include <math.h>
#include <stdio.h>

#include "config/AppConfig.h"

namespace {

float clampFloat(float value, float lo, float hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

}  // namespace

void TrendWidget::render(EpdDisplay& display, const VarioData& data) {
  renderStatic(display);
  renderDynamic(display, data);
}

void TrendWidget::renderStatic(EpdDisplay& display) {
  display.fillRect(bounds_, AppConfig::kWhite);
  display.drawLine(bounds_.x, bounds_.y, bounds_.x + bounds_.width - 1, bounds_.y, AppConfig::kBlack);

  display.drawSmallTextBold("VARIO HISTORY", bounds_.x + 20, bounds_.y + 12, 1, AppConfig::kBlack);
  display.drawSmallTextBold("SIM DATA", bounds_.x + bounds_.width - 86, bounds_.y + 12, 1, AppConfig::kBlack);

  const int32_t graphX = bounds_.x + 20;
  const int32_t graphY = bounds_.y + 38;
  const int32_t graphW = bounds_.width - 40;
  const int32_t graphH = bounds_.height - 54;
  const int32_t zeroY = graphY + graphH / 2;

  display.drawRect({graphX, graphY, graphW, graphH}, AppConfig::kLight);
  display.drawLine(graphX, zeroY, graphX + graphW - 1, zeroY, AppConfig::kMid);
}

void TrendWidget::renderDynamic(EpdDisplay& display, const VarioData& data) {
  const int32_t graphX = bounds_.x + 20;
  const int32_t graphY = bounds_.y + 38;
  const int32_t graphW = bounds_.width - 40;
  const int32_t graphH = bounds_.height - 54;
  const int32_t zeroY = graphY + graphH / 2;

  if (data.historyCount < 2) {
    return;
  }

  int32_t prevX = graphX;
  int32_t prevY = zeroY;
  const uint8_t start = kVarioHistorySamples - data.historyCount;
  for (uint8_t i = 0; i < data.historyCount; ++i) {
    const float sample = clampFloat(data.varioHistory[start + i], -4.0F, 4.0F);
    const int32_t x = graphX + static_cast<int32_t>((static_cast<float>(i) / (data.historyCount - 1)) * (graphW - 1));
    const int32_t y = zeroY - static_cast<int32_t>((sample / 4.0F) * (graphH / 2 - 3));
    if (i > 0) {
      display.drawLine(prevX, prevY, x, y, AppConfig::kBlack);
      display.drawLine(prevX, prevY + 1, x, y + 1, AppConfig::kBlack);
    }
    prevX = x;
    prevY = y;
  }
}

Rect_t TrendWidget::dynamicBounds() const {
  return {bounds_.x + 20, bounds_.y + 38, bounds_.width - 40, bounds_.height - 54};
}
