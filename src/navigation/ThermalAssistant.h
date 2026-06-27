#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "data/VarioData.h"

class ThermalAssistant {
 public:
  bool begin();
  void reset(VarioData& data);
  void update(VarioData& data,
              float currentLatitude,
              float currentLongitude,
              float currentLiftMs,
              float gpsCourseDeg,
              float windSpeedMs,
              float windDirectionDeg,
              bool isCircling);

 private:
  static constexpr uint16_t kFallbackSampleCapacity = kThermalAssistPoints;
  static constexpr uint16_t kPsramSampleCapacity = 192;
  static constexpr uint16_t kFallbackParticleCapacity = 16;
  static constexpr uint16_t kPsramParticleCapacity = 96;

  struct Sample {
    float eastM = 0.0F;
    float northM = 0.0F;
    float altitudeM = 0.0F;
    float liftMs = 0.0F;
    uint32_t timestampMs = 0;
    bool valid = false;
  };

  struct Particle {
    float eastM = 0.0F;
    float northM = 0.0F;
    float weight = 0.0F;
    float liftMs = 0.0F;
    uint32_t timestampMs = 0;
    bool valid = false;
  };

  struct HistoryThermal {
    float eastM = 0.0F;
    float northM = 0.0F;
    float coreMs = 0.0F;
    uint8_t confidencePercent = 0;
    uint32_t firstSeenMs = 0;
    uint32_t lastSeenMs = 0;
    bool active = false;
    bool valid = false;
  };

  Sample fallbackSamples_[kFallbackSampleCapacity] = {};
  Particle fallbackParticles_[kFallbackParticleCapacity] = {};
  HistoryThermal fallbackHistory_[kThermalHistoryPoints] = {};
  Sample* samples_ = fallbackSamples_;
  Particle* particles_ = fallbackParticles_;
  HistoryThermal* history_ = fallbackHistory_;
  uint16_t sampleCapacity_ = kFallbackSampleCapacity;
  uint16_t particleCapacity_ = kFallbackParticleCapacity;
  bool psramReady_ = false;
  bool buffersInitialized_ = false;

  uint16_t nextIndex_ = 0;
  uint16_t sampleCount_ = 0;
  bool anchorValid_ = false;
  float anchorLatDeg_ = 0.0F;
  float anchorLonDeg_ = 0.0F;
  float anchorCosLat_ = 1.0F;
  uint32_t lastUpdateMs_ = 0;
  uint32_t lastSampleMs_ = 0;
  float lastSampleEastM_ = 0.0F;
  float lastSampleNorthM_ = 0.0F;
  bool lastSamplePositionValid_ = false;
  float smoothedRangeM_ = 120.0F;
  float smoothedDriftDeg_ = 0.0F;
  float smoothedCoreEastM_ = 0.0F;
  float smoothedCoreNorthM_ = 0.0F;
  bool coreValid_ = false;
  bool driftValid_ = false;
  uint32_t lastCirclingMs_ = 0;
  bool wasCircling_ = false;
  uint8_t currentHistorySlot_ = 255;

  bool ensureBuffers();
  bool projectToLocal(float latitudeDeg, float longitudeDeg, float& eastM, float& northM);
  void applyWindDrift(float windSpeedMs, float windDirectionDeg, float dtSeconds, float altitudeM, ThermalAssistDriftMode mode);
  void addSample(float eastM, float northM, float altitudeM, float liftMs, uint32_t nowMs);
  void updateParticles(float eastM, float northM, float liftMs, uint32_t nowMs, bool addNewSample);
  void expireOldSamples(uint32_t nowMs);
  void expireHistory(uint32_t nowMs);
  void updateHistory(float centerEastM, float centerNorthM, float coreMs, uint8_t confidence, uint32_t nowMs, bool activeThermal);
  void populateHistoryOutput(VarioData& data, float originEastM, float originNorthM, uint32_t nowMs) const;
  void populateOutput(VarioData& data, float currentEastM, float currentNorthM, bool currentPositionValid, bool isCircling);
};
