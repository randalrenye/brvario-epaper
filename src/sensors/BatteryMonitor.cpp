#include "sensors/BatteryMonitor.h"

#include <epd_driver.h>

#include "config/HardwareConfig.h"

namespace {

struct BatteryPoint {
  float voltage;
  uint8_t percent;
};

const BatteryPoint kLipoCurve[] = {
    {4.20F, 100},
    {4.10F, 90},
    {4.00F, 80},
    {3.92F, 70},
    {3.85F, 60},
    {3.80F, 50},
    {3.75F, 40},
    {3.70F, 30},
    {3.60F, 20},
    {3.50F, 10},
    {3.30F, 0},
};

float clampFloat(float value, float lo, float hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

}  // namespace

bool BatteryMonitor::begin() {
  pinMode(HardwareConfig::kBatteryAdcPin, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(HardwareConfig::kBatteryAdcPin, ADC_11db);

  lastReadMs_ = 0;
  filteredVoltage_ = 0.0F;
  trendVoltage_ = 0.0F;
  percent_ = 0;
  chargingScore_ = 0;
  charging_ = false;
  trendValid_ = false;
  valid_ = false;
  update();
  Serial.printf("Bateria ADC iniciado no GPIO%d.\n", HardwareConfig::kBatteryAdcPin);
  return true;
}

bool BatteryMonitor::update() {
  const uint32_t now = millis();
  if (lastReadMs_ != 0 && now - lastReadMs_ < kReadIntervalMs) {
    return false;
  }
  lastReadMs_ = now;

  const float measured = readVoltage();
  if (measured < kMinValidVoltage) {
    valid_ = false;
    filteredVoltage_ = measured > 0.0F ? measured : 0.0F;
    percent_ = 0;
    charging_ = false;
    chargingScore_ = 0;
    trendValid_ = false;
    return true;
  }

  const float clamped = clampFloat(measured, kMinValidVoltage, kMaxReportedVoltage);
  updateChargingTrend(clamped);
  if (!valid_) {
    filteredVoltage_ = clamped;
    valid_ = true;
  } else {
    filteredVoltage_ = filteredVoltage_ * (1.0F - kFilterAlpha) + clamped * kFilterAlpha;
  }
  percent_ = voltageToPercent(filteredVoltage_);
  return true;
}

void BatteryMonitor::applyTo(VarioData& data) const {
  data.batteryVoltage = filteredVoltage_;
  data.batteryPercent = percent_;
  data.batteryCharging = valid_ && charging_;
}

float BatteryMonitor::readVoltage() {
  // LilyGo notes that EPD power must be enabled for a stable battery ADC read.
  epd_poweron();
  delay(8);

  uint32_t millivolts = 0;
  for (uint8_t i = 0; i < kSampleCount; ++i) {
    millivolts += analogReadMilliVolts(HardwareConfig::kBatteryAdcPin);
    delay(2);
  }
  epd_poweroff();

  const float adcVoltage = (static_cast<float>(millivolts) / static_cast<float>(kSampleCount)) / 1000.0F;
  return adcVoltage * HardwareConfig::kBatteryVoltageDivider * HardwareConfig::kBatteryCalibration;
}

void BatteryMonitor::updateChargingTrend(float voltage) {
  if (!trendValid_) {
    trendVoltage_ = voltage;
    trendValid_ = true;
    chargingScore_ = 0;
    charging_ = false;
    return;
  }

  const float delta = voltage - trendVoltage_;
  if (delta >= kFastRiseVoltage) {
    chargingScore_ += 4;
  } else if (delta >= kSmallRiseVoltage) {
    chargingScore_ += 2;
  } else if (delta <= kFallVoltage) {
    chargingScore_ -= 3;
  } else if (chargingScore_ > 0) {
    --chargingScore_;
  }

  if (chargingScore_ < 0) {
    chargingScore_ = 0;
  }
  if (chargingScore_ > kChargingScoreMax) {
    chargingScore_ = kChargingScoreMax;
  }

  if (!charging_ && chargingScore_ >= kChargingScoreOn) {
    charging_ = true;
  } else if (charging_ && chargingScore_ <= kChargingScoreOff) {
    charging_ = false;
  }

  trendVoltage_ = voltage;
}

uint8_t BatteryMonitor::voltageToPercent(float voltage) {
  if (voltage >= kLipoCurve[0].voltage) {
    return 100;
  }

  const uint8_t count = sizeof(kLipoCurve) / sizeof(kLipoCurve[0]);
  for (uint8_t i = 1; i < count; ++i) {
    const BatteryPoint& high = kLipoCurve[i - 1];
    const BatteryPoint& low = kLipoCurve[i];
    if (voltage >= low.voltage) {
      const float span = high.voltage - low.voltage;
      const float t = span > 0.0F ? (voltage - low.voltage) / span : 0.0F;
      const float percent = static_cast<float>(low.percent) + t * static_cast<float>(high.percent - low.percent);
      if (percent <= 0.0F) return 0;
      if (percent >= 100.0F) return 100;
      return static_cast<uint8_t>(percent + 0.5F);
    }
  }
  return 0;
}
