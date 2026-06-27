#include "network/WeatherWindCache.h"

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>

#include "network/OpenWeatherClient.h"

namespace {

static constexpr char kPrefsNamespace[] = "weatherWind";
static constexpr uint32_t kForecastHorizonSeconds = 3UL * 3600UL;
static constexpr uint32_t kDaySeconds = 24UL * 3600UL;
static constexpr uint32_t kFlightWindowStartSeconds = 9UL * 3600UL;
static constexpr uint32_t kFlightWindowEndSeconds = 17UL * 3600UL;
static constexpr float kMinForecastWindKmh = 0.5F;

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

float bearingFromVector(float east, float north) {
  return normalizeDeg(atan2f(east, north) * 57.295779513F);
}

uint32_t localDayStartUtc(uint32_t utc, int32_t timezoneOffsetSeconds) {
  const int64_t localTime = static_cast<int64_t>(utc) + static_cast<int64_t>(timezoneOffsetSeconds);
  const int64_t localDayStart = (localTime / static_cast<int64_t>(kDaySeconds)) * static_cast<int64_t>(kDaySeconds);
  return static_cast<uint32_t>(localDayStart - static_cast<int64_t>(timezoneOffsetSeconds));
}

void chooseWindWindow(uint32_t nowUtc,
                      int32_t timezoneOffsetSeconds,
                      uint32_t& windowStartUtc,
                      uint32_t& windowEndUtc) {
  const uint32_t dayStartUtc = localDayStartUtc(nowUtc, timezoneOffsetSeconds);
  const uint32_t todayFlightStartUtc = dayStartUtc + kFlightWindowStartSeconds;
  const uint32_t todayFlightEndUtc = dayStartUtc + kFlightWindowEndSeconds;

  if (nowUtc < todayFlightStartUtc) {
    windowStartUtc = todayFlightStartUtc;
  } else if (nowUtc >= todayFlightEndUtc) {
    windowStartUtc = todayFlightStartUtc + kDaySeconds;
  } else {
    windowStartUtc = nowUtc;
  }
  windowEndUtc = windowStartUtc + kForecastHorizonSeconds;
}

}  // namespace

bool WeatherWindCache::begin() {
  return load();
}

bool WeatherWindCache::updateFromWeather(const OpenWeatherClient& weather) {
  if (!weather.hasData() || weather.forecastDayCount() == 0) {
    return false;
  }

  const WeatherFlightData& current = weather.data(0);
  const uint32_t nowUtc = current.currentUtc;
  if (nowUtc == 0) {
    return false;
  }

  invalidate(false);

  uint32_t windowStartUtc = nowUtc;
  uint32_t windowEndUtc = nowUtc + kForecastHorizonSeconds;
  chooseWindWindow(nowUtc, current.timezoneOffsetSeconds, windowStartUtc, windowEndUtc);
  const bool currentWindow = windowStartUtc == nowUtc;

  float eastSum = 0.0F;
  float northSum = 0.0F;
  float speedSum = 0.0F;
  uint8_t count = 0;

  const auto addWindSample = [&](float speedKmh, int directionDeg) {
    if (speedKmh < kMinForecastWindKmh) {
      return;
    }
    const float windToDeg = normalizeDeg(static_cast<float>(directionDeg) + 180.0F);
    const float rad = windToDeg * 0.01745329252F;
    eastSum += sinf(rad) * speedKmh;
    northSum += cosf(rad) * speedKmh;
    speedSum += speedKmh;
    ++count;
  };

  if (currentWindow) {
    addWindSample(current.windSpeedKmh, current.windDirectionDeg);
  }

  for (uint8_t i = 0; i < weather.windForecastCount(); ++i) {
    const WeatherHourlyForecast& item = weather.windForecast(i);
    const bool afterWindowStart = currentWindow ? item.timeUtc > windowStartUtc : item.timeUtc >= windowStartUtc;
    if (!afterWindowStart || item.timeUtc > windowEndUtc) {
      continue;
    }
    addWindSample(item.windSpeedKmh, item.windDirectionDeg);
  }

  if (count == 0) {
    const WeatherHourlyForecast* nearest = nullptr;
    uint32_t nearestDistance = 0xFFFFFFFFUL;
    for (uint8_t i = 0; i < weather.windForecastCount(); ++i) {
      const WeatherHourlyForecast& item = weather.windForecast(i);
      if (item.windSpeedKmh < kMinForecastWindKmh) {
        continue;
      }
      const uint32_t distance =
          item.timeUtc > windowStartUtc ? item.timeUtc - windowStartUtc : windowStartUtc - item.timeUtc;
      if (distance <= kForecastHorizonSeconds && distance < nearestDistance) {
        nearest = &item;
        nearestDistance = distance;
      }
    }
    if (nearest) {
      addWindSample(nearest->windSpeedKmh, nearest->windDirectionDeg);
    } else if (currentWindow) {
      addWindSample(current.windSpeedKmh, current.windDirectionDeg);
    }
  }

  if (count == 0) {
    invalidate(true);
    return false;
  }

  speedKmh_ = clampFloat(speedSum / static_cast<float>(count), 0.0F, 120.0F);
  directionDeg_ = bearingFromVector(eastSum, northSum);
  valid_ = true;
  validFromUtc_ = nowUtc;
  validUntilUtc_ = windowEndUtc;
  updatedUtc_ = nowUtc;
  return save();
}

bool WeatherWindCache::hasValid(uint32_t utcNow) const {
  if (!valid_ || utcNow == 0 || speedKmh_ < kMinForecastWindKmh) {
    return false;
  }
  return utcNow >= validFromUtc_ && utcNow <= validUntilUtc_;
}

void WeatherWindCache::clear() {
  invalidate(true);
}

bool WeatherWindCache::save() const {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return false;
  }
  prefs.putUInt("magic", kMagic);
  prefs.putUShort("ver", kVersion);
  prefs.putBool("valid", valid_);
  prefs.putFloat("speed", speedKmh_);
  prefs.putFloat("dir", directionDeg_);
  prefs.putUInt("from", validFromUtc_);
  prefs.putUInt("until", validUntilUtc_);
  prefs.putUInt("updated", updatedUtc_);
  prefs.end();
  return true;
}

bool WeatherWindCache::load() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    invalidate(false);
    return false;
  }
  const uint32_t magic = prefs.getUInt("magic", 0);
  const uint16_t version = prefs.getUShort("ver", 0);
  if (magic != kMagic || version != kVersion) {
    prefs.end();
    invalidate(false);
    return false;
  }
  valid_ = prefs.getBool("valid", false);
  speedKmh_ = prefs.getFloat("speed", 0.0F);
  directionDeg_ = normalizeDeg(prefs.getFloat("dir", 0.0F));
  validFromUtc_ = prefs.getUInt("from", 0);
  validUntilUtc_ = prefs.getUInt("until", 0);
  updatedUtc_ = prefs.getUInt("updated", 0);
  prefs.end();

  if (validUntilUtc_ <= validFromUtc_ || speedKmh_ < kMinForecastWindKmh) {
    invalidate(false);
    return false;
  }
  return valid_;
}

void WeatherWindCache::invalidate(bool persist) {
  valid_ = false;
  speedKmh_ = 0.0F;
  directionDeg_ = 0.0F;
  validFromUtc_ = 0;
  validUntilUtc_ = 0;
  updatedUtc_ = 0;
  if (persist) {
    save();
  }
}
