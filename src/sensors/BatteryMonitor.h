#pragma once

#include <Arduino.h>

#include "data/VarioData.h"

class BatteryMonitor {
 public:
  bool begin();
  bool update();
  void applyTo(VarioData& data) const;

  float voltage() const { return filteredVoltage_; }
  uint8_t percent() const { return percent_; }
  bool charging() const { return charging_; }
  bool valid() const { return valid_; }

 private:
  static constexpr uint32_t kReadIntervalMs = 5000UL;
  static constexpr uint8_t kSampleCount = 8;
  static constexpr float kFilterAlpha = 0.18F;
  static constexpr float kMinValidVoltage = 2.50F;
  static constexpr float kMaxReportedVoltage = 4.25F;
  static constexpr float kSmallRiseVoltage = 0.004F;
  static constexpr float kFastRiseVoltage = 0.020F;
  static constexpr float kFallVoltage = -0.006F;
  static constexpr int8_t kChargingScoreOn = 3;
  static constexpr int8_t kChargingScoreOff = 1;
  static constexpr int8_t kChargingScoreMax = 8;

  bool valid_ = false;
  bool charging_ = false;
  bool trendValid_ = false;
  uint32_t lastReadMs_ = 0;
  float filteredVoltage_ = 0.0F;
  float trendVoltage_ = 0.0F;
  uint8_t percent_ = 0;
  int8_t chargingScore_ = 0;

  float readVoltage();
  void updateChargingTrend(float voltage);
  static uint8_t voltageToPercent(float voltage);
};
