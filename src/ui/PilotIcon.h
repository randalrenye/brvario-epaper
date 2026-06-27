#pragma once

#include <stdint.h>

#include "display/EpdDisplay.h"

namespace PilotIcon {

void draw(EpdDisplay& display,
          int32_t cx,
          int32_t cy,
          float bearingDeg,
          int32_t frontLength,
          int32_t rearLength,
          int32_t halfWidth,
          uint8_t color);

void draw(uint8_t* framebuffer,
          int32_t cx,
          int32_t cy,
          float bearingDeg,
          int32_t frontLength,
          int32_t rearLength,
          int32_t halfWidth,
          uint8_t color);

}  // namespace PilotIcon
