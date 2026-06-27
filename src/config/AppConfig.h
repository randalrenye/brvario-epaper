#pragma once

#include <stdint.h>

namespace AppConfig {

static constexpr uint32_t kUiUpdateIntervalMs = 550;
static constexpr uint16_t kFullRefreshEveryPartialCycles = 900;
static constexpr uint8_t kWhiteErasePasses = 1;
static constexpr uint8_t kLowerBandContrastEveryCycles = 0;
static constexpr uint8_t kGlobalContrastEveryCycles = 24;
static constexpr uint8_t kContrastReinforcePasses = 2;
static constexpr uint8_t kTransitionContrastPasses = 1;
static constexpr int32_t kDirtyPaddingPx = 6;
static constexpr uint8_t kTrendUpdateEveryCycles = 10;

static constexpr uint8_t kBlack = 0x00;
static constexpr uint8_t kDark = 0x00;
static constexpr uint8_t kMid = 0x44;
static constexpr uint8_t kLight = 0xAA;
static constexpr uint8_t kWhite = 0xFF;

static constexpr uint8_t kSmallTextScale = 2;
static constexpr uint8_t kLabelTextScale = 1;

}  // namespace AppConfig
