#include "navigation/FlightMetrics.h"

#include <math.h>
#include <string.h>

namespace {

static constexpr float kDegToRad = 0.01745329252F;
static constexpr float kEarthRadiusM = 6371000.0F;

bool validPosition(float latDeg, float lonDeg) {
  return isfinite(latDeg) && isfinite(lonDeg) && latDeg >= -90.0F && latDeg <= 90.0F && lonDeg >= -180.0F && lonDeg <= 180.0F;
}

}  // namespace

void FlightMetrics::reset(VarioData& data) {
  thermalActive_ = false;
  thermalExitStartedMs_ = 0;
  thermalBaseAltitudeM_ = 0.0F;
  thermalGainM_ = 0.0F;
  lastNavigationValid_ = false;
  lastNavigationTimestampMs_ = 0;
  lastLatitudeDeg_ = 0.0F;
  lastLongitudeDeg_ = 0.0F;
  lastAltitudeM_ = 0.0F;
  clearGlide();
  smoothedGlideRatio_ = 0.0F;
  data.ganhoTermicaM = 0.0F;
  data.glideRatio = 0.0F;
}

void FlightMetrics::update(VarioData& data, uint32_t navigationTimestampMs, bool isCircling) {
  const uint32_t now = millis();
  updateThermalGain(data, isCircling, now);
  updateGlideRatio(data, navigationTimestampMs, isCircling);
}

void FlightMetrics::updateThermalGain(VarioData& data, bool isCircling, uint32_t nowMs) {
  if (!data.sensorDataValid || !isfinite(data.altitudeM) || !isfinite(data.varioMs)) {
    thermalActive_ = false;
    thermalExitStartedMs_ = 0;
    thermalGainM_ = 0.0F;
    data.ganhoTermicaM = 0.0F;
    return;
  }

  const bool thermalCandidate = isCircling && data.varioMs >= kThermalEntryMinVarioMs;
  if (thermalCandidate) {
    if (!thermalActive_) {
      thermalActive_ = true;
      thermalBaseAltitudeM_ = data.altitudeM;
      thermalGainM_ = 0.0F;
      clearGlide();
    }

    thermalExitStartedMs_ = 0;
    if (data.altitudeM < thermalBaseAltitudeM_) {
      thermalBaseAltitudeM_ = data.altitudeM;
    }

    const float currentGainM = data.altitudeM - thermalBaseAltitudeM_;
    if (currentGainM > thermalGainM_) {
      thermalGainM_ = currentGainM;
    }
    thermalGainM_ = clampFloat(thermalGainM_, 0.0F, 9999.0F);
    data.ganhoTermicaM = thermalGainM_;
    return;
  }

  if (!thermalActive_) {
    thermalGainM_ = 0.0F;
    data.ganhoTermicaM = 0.0F;
    return;
  }

  if (thermalExitStartedMs_ == 0) {
    thermalExitStartedMs_ = nowMs;
  }

  if (nowMs - thermalExitStartedMs_ >= kThermalExitGraceMs) {
    thermalActive_ = false;
    thermalExitStartedMs_ = 0;
    thermalGainM_ = 0.0F;
    data.ganhoTermicaM = 0.0F;
    return;
  }

  // Keep the last thermal gain briefly while the pilot finishes leaving the turn.
  data.ganhoTermicaM = thermalGainM_;
}

void FlightMetrics::updateGlideRatio(VarioData& data, uint32_t navigationTimestampMs, bool isCircling) {
  if (!data.gpsFix || !data.sensorDataValid || navigationTimestampMs == 0 || !validPosition(data.latitudeDeg, data.longitudeDeg) ||
      !isfinite(data.altitudeM) || !isfinite(data.groundSpeedKmh) || !isfinite(data.varioMs) || isCircling ||
      data.groundSpeedKmh < kMinGlideSpeedKmh || data.groundSpeedKmh > kMaxGlideSpeedKmh) {
    lastNavigationValid_ = false;
    clearGlide();
    smoothedGlideRatio_ = 0.0F;
    data.glideRatio = 0.0F;
    return;
  }

  if (navigationTimestampMs == lastNavigationTimestampMs_) {
    data.glideRatio = smoothedGlideRatio_;
    return;
  }

  if (!lastNavigationValid_) {
    lastNavigationValid_ = true;
    lastNavigationTimestampMs_ = navigationTimestampMs;
    lastLatitudeDeg_ = data.latitudeDeg;
    lastLongitudeDeg_ = data.longitudeDeg;
    lastAltitudeM_ = data.altitudeM;
    data.glideRatio = smoothedGlideRatio_;
    return;
  }

  const uint32_t dtMs = navigationTimestampMs - lastNavigationTimestampMs_;
  if (dtMs == 0 || dtMs > kMaxNavigationGapMs) {
    lastNavigationTimestampMs_ = navigationTimestampMs;
    lastLatitudeDeg_ = data.latitudeDeg;
    lastLongitudeDeg_ = data.longitudeDeg;
    lastAltitudeM_ = data.altitudeM;
    data.glideRatio = smoothedGlideRatio_;
    return;
  }

  const float dt = static_cast<float>(dtMs) / 1000.0F;
  const float distanceM = distanceMeters(lastLatitudeDeg_, lastLongitudeDeg_, data.latitudeDeg, data.longitudeDeg);
  const float baroLossM = lastAltitudeM_ - data.altitudeM;
  const float varioLossM = data.varioMs < kSinkGateVarioMs ? -data.varioMs * dt : 0.0F;
  float lossM = 0.0F;
  if (baroLossM > 0.15F && varioLossM > 0.0F) {
    lossM = baroLossM * 0.70F + varioLossM * 0.30F;
  } else if (baroLossM > 0.25F) {
    lossM = baroLossM;
  } else if (varioLossM > 0.15F) {
    lossM = varioLossM;
  }

  if (distanceM > 0.5F && distanceM <= kMaxGpsStepM && lossM > 0.05F) {
    addGlideSegment(navigationTimestampMs, distanceM, lossM);
  }

  float windowDistanceM = 0.0F;
  float windowLossM = 0.0F;
  if (sumRecentGlide(navigationTimestampMs, windowDistanceM, windowLossM) && windowDistanceM >= kMinGlideDistanceM &&
      windowLossM >= kMinGlideLossM) {
    const float rawGlide = clampFloat(windowDistanceM / windowLossM, 0.0F, 99.9F);
    smoothedGlideRatio_ = smoothedGlideRatio_ <= 0.1F ? rawGlide : smoothedGlideRatio_ * 0.78F + rawGlide * 0.22F;
  } else {
    smoothedGlideRatio_ = 0.0F;
  }

  data.glideRatio = smoothedGlideRatio_;
  lastNavigationTimestampMs_ = navigationTimestampMs;
  lastLatitudeDeg_ = data.latitudeDeg;
  lastLongitudeDeg_ = data.longitudeDeg;
  lastAltitudeM_ = data.altitudeM;
}

void FlightMetrics::clearGlide() {
  memset(glideSegments_, 0, sizeof(glideSegments_));
  glideHead_ = 0;
  glideCount_ = 0;
}

void FlightMetrics::addGlideSegment(uint32_t timestampMs, float distanceM, float lossM) {
  glideSegments_[glideHead_].timestampMs = timestampMs;
  glideSegments_[glideHead_].distanceM = distanceM;
  glideSegments_[glideHead_].lossM = lossM;
  glideHead_ = static_cast<uint8_t>((glideHead_ + 1) % kMaxGlideSegments);
  if (glideCount_ < kMaxGlideSegments) {
    ++glideCount_;
  }
}

bool FlightMetrics::sumRecentGlide(uint32_t nowMs, float& distanceM, float& lossM) const {
  distanceM = 0.0F;
  lossM = 0.0F;

  for (uint8_t i = 0; i < glideCount_; ++i) {
    const GlideSegment& segment = glideSegments_[i];
    if (segment.timestampMs == 0 || nowMs - segment.timestampMs > kGlideWindowMs) {
      continue;
    }
    distanceM += segment.distanceM;
    lossM += segment.lossM;
  }

  return lossM > 0.0F && distanceM > 0.0F;
}

float FlightMetrics::distanceMeters(float lat1Deg, float lon1Deg, float lat2Deg, float lon2Deg) {
  const float lat1 = lat1Deg * kDegToRad;
  const float lat2 = lat2Deg * kDegToRad;
  const float dLat = (lat2Deg - lat1Deg) * kDegToRad;
  const float dLon = (lon2Deg - lon1Deg) * kDegToRad;
  const float meanLat = (lat1 + lat2) * 0.5F;
  const float x = dLon * cosf(meanLat);
  const float y = dLat;
  return sqrtf(x * x + y * y) * kEarthRadiusM;
}

float FlightMetrics::clampFloat(float value, float lo, float hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}
