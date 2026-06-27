#pragma once

#include <Arduino.h>

#include "data/VarioData.h"

class FlightMetrics {
 public:
  void reset(VarioData& data);
  void update(VarioData& data, uint32_t navigationTimestampMs, bool isCircling);

 private:
  struct GlideSegment {
    uint32_t timestampMs = 0;
    float distanceM = 0.0F;
    float lossM = 0.0F;
  };

  static constexpr uint8_t kMaxGlideSegments = 72;
  static constexpr uint32_t kThermalExitGraceMs = 60000UL;
  static constexpr uint32_t kGlideWindowMs = 90000UL;
  static constexpr uint32_t kMaxNavigationGapMs = 6000UL;
  static constexpr float kMinGlideSpeedKmh = 6.0F;
  static constexpr float kMaxGlideSpeedKmh = 90.0F;
  static constexpr float kMinGlideDistanceM = 35.0F;
  static constexpr float kMinGlideLossM = 5.0F;
  static constexpr float kMaxGpsStepM = 250.0F;
  static constexpr float kThermalEntryMinVarioMs = -0.45F;
  static constexpr float kSinkGateVarioMs = -0.06F;

  bool thermalActive_ = false;
  uint32_t thermalExitStartedMs_ = 0;
  float thermalBaseAltitudeM_ = 0.0F;
  float thermalGainM_ = 0.0F;

  bool lastNavigationValid_ = false;
  uint32_t lastNavigationTimestampMs_ = 0;
  float lastLatitudeDeg_ = 0.0F;
  float lastLongitudeDeg_ = 0.0F;
  float lastAltitudeM_ = 0.0F;

  GlideSegment glideSegments_[kMaxGlideSegments] = {};
  uint8_t glideHead_ = 0;
  uint8_t glideCount_ = 0;
  float smoothedGlideRatio_ = 0.0F;

  void updateThermalGain(VarioData& data, bool isCircling, uint32_t nowMs);
  void updateGlideRatio(VarioData& data, uint32_t navigationTimestampMs, bool isCircling);
  void clearGlide();
  void addGlideSegment(uint32_t timestampMs, float distanceM, float lossM);
  bool sumRecentGlide(uint32_t nowMs, float& distanceM, float& lossM) const;
  static float distanceMeters(float lat1Deg, float lon1Deg, float lat2Deg, float lon2Deg);
  static float clampFloat(float value, float lo, float hi);
};
