#include "ui/PilotIcon.h"

#include <math.h>

#include "epd_driver.h"

namespace {

static constexpr float kDegToRad = 0.01745329251994329577F;

float normalizeDeg(float value) {
  while (value < 0.0F) value += 360.0F;
  while (value >= 360.0F) value -= 360.0F;
  return value;
}

int32_t alongX(float bearingDeg, int32_t length) {
  return static_cast<int32_t>(sinf(normalizeDeg(bearingDeg) * kDegToRad) * static_cast<float>(length));
}

int32_t alongY(float bearingDeg, int32_t length) {
  return static_cast<int32_t>(-cosf(normalizeDeg(bearingDeg) * kDegToRad) * static_cast<float>(length));
}

struct ArrowPoints {
  int32_t tipX;
  int32_t tipY;
  int32_t leftX;
  int32_t leftY;
  int32_t notchX;
  int32_t notchY;
  int32_t rightX;
  int32_t rightY;
};

ArrowPoints makeArrow(int32_t cx, int32_t cy, float bearingDeg, int32_t frontLength, int32_t rearLength, int32_t halfWidth) {
  const float leftDeg = normalizeDeg(bearingDeg - 90.0F);
  const float rightDeg = normalizeDeg(bearingDeg + 90.0F);
  const int32_t notchBack = rearLength / 3;

  ArrowPoints p;
  p.tipX = cx + alongX(bearingDeg, frontLength);
  p.tipY = cy + alongY(bearingDeg, frontLength);
  p.notchX = cx - alongX(bearingDeg, notchBack);
  p.notchY = cy - alongY(bearingDeg, notchBack);
  p.leftX = cx - alongX(bearingDeg, rearLength) + alongX(leftDeg, halfWidth);
  p.leftY = cy - alongY(bearingDeg, rearLength) + alongY(leftDeg, halfWidth);
  p.rightX = cx - alongX(bearingDeg, rearLength) + alongX(rightDeg, halfWidth);
  p.rightY = cy - alongY(bearingDeg, rearLength) + alongY(rightDeg, halfWidth);
  return p;
}

}  // namespace

namespace PilotIcon {

void draw(EpdDisplay& display,
          int32_t cx,
          int32_t cy,
          float bearingDeg,
          int32_t frontLength,
          int32_t rearLength,
          int32_t halfWidth,
          uint8_t color) {
  const ArrowPoints p = makeArrow(cx, cy, bearingDeg, frontLength, rearLength, halfWidth);
  display.fillTriangle(p.tipX, p.tipY, p.leftX, p.leftY, p.notchX, p.notchY, color);
  display.fillTriangle(p.tipX, p.tipY, p.notchX, p.notchY, p.rightX, p.rightY, color);
}

void draw(uint8_t* framebuffer,
          int32_t cx,
          int32_t cy,
          float bearingDeg,
          int32_t frontLength,
          int32_t rearLength,
          int32_t halfWidth,
          uint8_t color) {
  if (!framebuffer) return;
  const ArrowPoints p = makeArrow(cx, cy, bearingDeg, frontLength, rearLength, halfWidth);
  epd_fill_triangle(p.tipX, p.tipY, p.leftX, p.leftY, p.notchX, p.notchY, color, framebuffer);
  epd_fill_triangle(p.tipX, p.tipY, p.notchX, p.notchY, p.rightX, p.rightY, color, framebuffer);
}

}  // namespace PilotIcon
