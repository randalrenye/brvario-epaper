#pragma once

#include <stdint.h>

namespace HardwareConfig {

// ESP32-S3 native UART0 pins, used here through Serial1 remap.
// Wiring:
// GPS TX -> ESP32 GPIO44 (RX0)
// GPS RX -> ESP32 GPIO43 (TX0), optional for GPS configuration
static constexpr int kGpsRxPin = 44;
static constexpr int kGpsTxPin = 43;
static constexpr uint32_t kGpsBaud = 9600;

// Passive buzzer / piezo on the 40-pin header pin marked CS / IO39.
// This is GPIO_CS from utilities.h, not the TF-card SD_CS pin (IO42).
static constexpr int kBuzzerPin = 39;

// Internal LilyGo battery ADC pin. The board uses a 2:1 divider before ADC.
static constexpr int kBatteryAdcPin = 14;
static constexpr float kBatteryVoltageDivider = 2.0F;
static constexpr float kBatteryCalibration = 1.0F;

}  // namespace HardwareConfig
