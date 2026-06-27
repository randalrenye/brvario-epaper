#pragma once

#include <stdint.h>

class OpenWeatherClient;

class WeatherWindCache {
 public:
  bool begin();
  bool updateFromWeather(const OpenWeatherClient& weather);
  bool hasValid(uint32_t utcNow) const;
  void clear();

  float speedKmh() const { return speedKmh_; }
  float directionDeg() const { return directionDeg_; }
  uint32_t validUntilUtc() const { return validUntilUtc_; }

 private:
  static constexpr uint32_t kMagic = 0x42525744UL;  // "BRWD"
  static constexpr uint16_t kVersion = 3;

  bool valid_ = false;
  float speedKmh_ = 0.0F;
  float directionDeg_ = 0.0F;
  uint32_t validFromUtc_ = 0;
  uint32_t validUntilUtc_ = 0;
  uint32_t updatedUtc_ = 0;

  bool save() const;
  bool load();
  void invalidate(bool persist);
};
