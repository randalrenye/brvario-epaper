#include "navigation/ThermalAssistant.h"

#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

namespace {

static constexpr float kEarthRadiusM = 6371000.0F;
static constexpr float kDegToRad = 0.01745329252F;
static constexpr float kRadToDeg = 57.295779513F;
static constexpr uint32_t kSampleIntervalMs = 1000UL;
static constexpr float kMinSampleSpacingM = 3.0F;
static constexpr float kMinRangeM = 60.0F;
static constexpr float kMaxRangeM = 280.0F;
static constexpr float kRangeMargin = 1.25F;
static constexpr float kRangeAlpha = 0.20F;
static constexpr float kDriftAlpha = 0.25F;
static constexpr float kMinCoreVectorM = 4.0F;
static constexpr uint32_t kLostThermalResetMs = 6UL * 60UL * 1000UL;
static constexpr uint32_t kSampleMaxAgeMs = 6UL * 60UL * 1000UL;
static constexpr uint32_t kHistoryMaxAgeMs = 45UL * 60UL * 1000UL;
static constexpr float kHistoryMergeDistanceM = 140.0F;
static constexpr float kParticleMergeDistanceM = 28.0F;
static constexpr float kParticleMinWeight = 0.05F;
static constexpr float kParticleDecayPerSecond = 0.982F;
static constexpr float kCoreClassicAlpha = 0.22F;
static constexpr float kCoreParticleAlpha = 0.30F;
static constexpr float kMinThermalSampleLiftMs = 0.10F;
static constexpr uint8_t kMaxVisibleThermalPoints = 12;
static constexpr float kVisiblePointSpacingM = 9.0F;
static constexpr float kVisiblePointReplacementLiftMs = 0.35F;

float clampFloat(float value, float lo, float hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

uint8_t clampPercent(float value) {
  return static_cast<uint8_t>(clampFloat(value, 0.0F, 100.0F) + 0.5F);
}

float normalizeDeg(float value) {
  while (value < 0.0F) value += 360.0F;
  while (value >= 360.0F) value -= 360.0F;
  return value;
}

float shortestAngleDelta(float fromDeg, float toDeg) {
  float delta = normalizeDeg(toDeg) - normalizeDeg(fromDeg);
  if (delta > 180.0F) delta -= 360.0F;
  if (delta < -180.0F) delta += 360.0F;
  return delta;
}

float blendAngle(float currentDeg, float targetDeg, float alpha) {
  return normalizeDeg(currentDeg + shortestAngleDelta(currentDeg, targetDeg) * alpha);
}

float bearingFromVector(float eastM, float northM) {
  if (fabsf(eastM) < 0.01F && fabsf(northM) < 0.01F) {
    return 0.0F;
  }
  return normalizeDeg(atan2f(eastM, northM) * kRadToDeg);
}

float liftWeight(float liftMs) {
  const float positiveLift = clampFloat(liftMs, 0.0F, 5.0F);
  return 0.08F + positiveLift * positiveLift;
}

bool isThermalLift(float liftMs) {
  return isfinite(liftMs) && liftMs > kMinThermalSampleLiftMs;
}

float distanceM(float eastA, float northA, float eastB, float northB) {
  const float de = eastA - eastB;
  const float dn = northA - northB;
  return sqrtf(de * de + dn * dn);
}

float altitudeDriftFactor(float sampleAltitudeM, float referenceAltitudeM) {
  if (!isfinite(sampleAltitudeM) || !isfinite(referenceAltitudeM)) {
    return 1.0F;
  }
  return clampFloat(1.0F + (sampleAltitudeM - referenceAltitudeM) / 2500.0F, 0.86F, 1.18F);
}

float positiveAgeSeconds(uint32_t nowMs, uint32_t thenMs, float maxSeconds) {
  if (thenMs == 0 || nowMs < thenMs) {
    return 0.0F;
  }
  return clampFloat(static_cast<float>(nowMs - thenMs) * 0.001F, 0.0F, maxSeconds);
}

}  // namespace

bool ThermalAssistant::begin() {
  const bool usingPsram = ensureBuffers();
  Serial.printf("Assistente termica: %s (%u amostras, %u particulas, %u historicos).\n",
                usingPsram ? "PSRAM ativa" : "fallback RAM interna",
                static_cast<unsigned>(sampleCapacity_),
                static_cast<unsigned>(particleCapacity_),
                static_cast<unsigned>(kThermalHistoryPoints));
  return usingPsram;
}

bool ThermalAssistant::ensureBuffers() {
  if (buffersInitialized_) {
    return psramReady_;
  }
  buffersInitialized_ = true;

  Sample* psramSamples =
      static_cast<Sample*>(heap_caps_malloc(sizeof(Sample) * kPsramSampleCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  Particle* psramParticles =
      static_cast<Particle*>(heap_caps_malloc(sizeof(Particle) * kPsramParticleCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  HistoryThermal* psramHistory =
      static_cast<HistoryThermal*>(heap_caps_malloc(sizeof(HistoryThermal) * kThermalHistoryPoints, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

  if (psramSamples && psramParticles && psramHistory) {
    samples_ = psramSamples;
    particles_ = psramParticles;
    history_ = psramHistory;
    sampleCapacity_ = kPsramSampleCapacity;
    particleCapacity_ = kPsramParticleCapacity;
    psramReady_ = true;
  } else {
    if (psramSamples) heap_caps_free(psramSamples);
    if (psramParticles) heap_caps_free(psramParticles);
    if (psramHistory) heap_caps_free(psramHistory);
    samples_ = fallbackSamples_;
    particles_ = fallbackParticles_;
    history_ = fallbackHistory_;
    sampleCapacity_ = kFallbackSampleCapacity;
    particleCapacity_ = kFallbackParticleCapacity;
    psramReady_ = false;
  }

  memset(samples_, 0, sizeof(Sample) * sampleCapacity_);
  memset(particles_, 0, sizeof(Particle) * particleCapacity_);
  memset(history_, 0, sizeof(HistoryThermal) * kThermalHistoryPoints);
  return psramReady_;
}

void ThermalAssistant::reset(VarioData& data) {
  ensureBuffers();
  memset(samples_, 0, sizeof(Sample) * sampleCapacity_);
  memset(particles_, 0, sizeof(Particle) * particleCapacity_);
  memset(history_, 0, sizeof(HistoryThermal) * kThermalHistoryPoints);
  nextIndex_ = 0;
  sampleCount_ = 0;
  anchorValid_ = false;
  lastUpdateMs_ = 0;
  lastSampleMs_ = 0;
  lastSamplePositionValid_ = false;
  smoothedRangeM_ = 120.0F;
  smoothedDriftDeg_ = normalizeDeg(data.courseDeg);
  smoothedCoreEastM_ = 0.0F;
  smoothedCoreNorthM_ = 0.0F;
  coreValid_ = false;
  driftValid_ = false;
  lastCirclingMs_ = 0;
  wasCircling_ = false;
  currentHistorySlot_ = 255;
  data.thermalPointCount = 0;
  data.thermalHistoryCount = 0;
  data.thermalRangeM = smoothedRangeM_;
  data.thermalDriftDeg = smoothedDriftDeg_;
  data.thermalPilotEastM = 0.0F;
  data.thermalPilotNorthM = 0.0F;
  data.thermalCoreMs = 0.0F;
  data.thermalLockPercent = 0;
  data.thermalCoreConfidencePercent = 0;
  data.thermalDriftMode = ThermalAssistDriftMode::Classic;
}

void ThermalAssistant::update(VarioData& data,
                              float currentLatitude,
                              float currentLongitude,
                              float currentLiftMs,
                              float gpsCourseDeg,
                              float windSpeedMs,
                              float windDirectionDeg,
                              bool isCircling) {
  ensureBuffers();

  const uint32_t nowMs = millis();
  float dtSeconds = 0.0F;
  if (lastUpdateMs_ != 0 && nowMs >= lastUpdateMs_) {
    dtSeconds = static_cast<float>(nowMs - lastUpdateMs_) * 0.001F;
    if (dtSeconds > 3.0F) {
      dtSeconds = 3.0F;
    }
  }
  lastUpdateMs_ = nowMs;

  const ThermalAssistDriftMode driftMode = data.thermalDriftMode;
  applyWindDrift(windSpeedMs, windDirectionDeg, dtSeconds, data.altitudeM, driftMode);
  expireOldSamples(nowMs);
  expireHistory(nowMs);

  float currentEastM = 0.0F;
  float currentNorthM = 0.0F;
  const bool positionValid = projectToLocal(currentLatitude, currentLongitude, currentEastM, currentNorthM);
  const bool liftValid = isfinite(currentLiftMs);
  const bool thermalLift = isThermalLift(currentLiftMs);
  const bool readyToSample = positionValid && liftValid && thermalLift && isCircling;

  if (isCircling) {
    lastCirclingMs_ = nowMs;
  } else {
    if (wasCircling_ && currentHistorySlot_ < kThermalHistoryPoints && history_[currentHistorySlot_].valid) {
      history_[currentHistorySlot_].active = false;
      currentHistorySlot_ = 255;
    }

    if (sampleCount_ > 0 && lastCirclingMs_ != 0 && nowMs - lastCirclingMs_ > kLostThermalResetMs) {
      memset(samples_, 0, sizeof(Sample) * sampleCapacity_);
      memset(particles_, 0, sizeof(Particle) * particleCapacity_);
      nextIndex_ = 0;
      sampleCount_ = 0;
      lastSamplePositionValid_ = false;
      coreValid_ = false;
      driftValid_ = false;
    }
  }
  wasCircling_ = isCircling;

  bool spacingOk = true;
  if (lastSamplePositionValid_) {
    spacingOk = distanceM(currentEastM, currentNorthM, lastSampleEastM_, lastSampleNorthM_) >= kMinSampleSpacingM;
  }

  const bool sampled =
      readyToSample && (sampleCount_ == 0 || (nowMs - lastSampleMs_ >= kSampleIntervalMs && spacingOk));
  if (sampled) {
    addSample(currentEastM, currentNorthM, data.altitudeM, currentLiftMs, nowMs);
  }
  if (positionValid) {
    updateParticles(currentEastM, currentNorthM, thermalLift ? currentLiftMs : 0.0F, nowMs, sampled);
  }

  data.courseDeg = normalizeDeg(gpsCourseDeg);
  populateOutput(data, currentEastM, currentNorthM, positionValid, isCircling);
}

bool ThermalAssistant::projectToLocal(float latitudeDeg, float longitudeDeg, float& eastM, float& northM) {
  if (!isfinite(latitudeDeg) || !isfinite(longitudeDeg)) {
    return false;
  }
  if (fabsf(latitudeDeg) < 0.000001F && fabsf(longitudeDeg) < 0.000001F) {
    return false;
  }

  if (!anchorValid_) {
    anchorLatDeg_ = latitudeDeg;
    anchorLonDeg_ = longitudeDeg;
    anchorCosLat_ = cosf(anchorLatDeg_ * kDegToRad);
    if (fabsf(anchorCosLat_) < 0.01F) {
      anchorCosLat_ = anchorCosLat_ >= 0.0F ? 0.01F : -0.01F;
    }
    anchorValid_ = true;
  }

  northM = (latitudeDeg - anchorLatDeg_) * kDegToRad * kEarthRadiusM;
  eastM = (longitudeDeg - anchorLonDeg_) * kDegToRad * kEarthRadiusM * anchorCosLat_;
  return true;
}

void ThermalAssistant::applyWindDrift(float windSpeedMs,
                                      float windDirectionDeg,
                                      float dtSeconds,
                                      float altitudeM,
                                      ThermalAssistDriftMode mode) {
  if (dtSeconds <= 0.0F || !isfinite(windSpeedMs) || windSpeedMs <= 0.05F || !isfinite(windDirectionDeg)) {
    return;
  }

  // BRVARIO usa a direcao para onde a massa de ar se desloca.
  const float driftToDeg = normalizeDeg(windDirectionDeg);
  const float distance = clampFloat(windSpeedMs, 0.0F, 40.0F) * dtSeconds;
  const float eastShiftM = sinf(driftToDeg * kDegToRad) * distance;
  const float northShiftM = cosf(driftToDeg * kDegToRad) * distance;
  const float particleModeBoost = mode == ThermalAssistDriftMode::Particle ? 1.04F : 1.0F;

  for (uint16_t i = 0; i < sampleCapacity_; ++i) {
    if (!samples_[i].valid) continue;
    const float altitudeFactor = altitudeDriftFactor(samples_[i].altitudeM, altitudeM);
    samples_[i].eastM += eastShiftM * altitudeFactor;
    samples_[i].northM += northShiftM * altitudeFactor;
  }

  for (uint16_t i = 0; i < particleCapacity_; ++i) {
    if (!particles_[i].valid) continue;
    particles_[i].eastM += eastShiftM * particleModeBoost;
    particles_[i].northM += northShiftM * particleModeBoost;
  }

  for (uint8_t i = 0; i < kThermalHistoryPoints; ++i) {
    if (!history_[i].valid) continue;
    history_[i].eastM += eastShiftM;
    history_[i].northM += northShiftM;
  }

  if (lastSamplePositionValid_) {
    lastSampleEastM_ += eastShiftM;
    lastSampleNorthM_ += northShiftM;
  }
  if (coreValid_) {
    smoothedCoreEastM_ += eastShiftM;
    smoothedCoreNorthM_ += northShiftM;
  }
}

void ThermalAssistant::addSample(float eastM, float northM, float altitudeM, float liftMs, uint32_t nowMs) {
  Sample& sample = samples_[nextIndex_];
  sample.eastM = eastM;
  sample.northM = northM;
  sample.altitudeM = altitudeM;
  sample.liftMs = clampFloat(liftMs, -5.0F, 8.0F);
  sample.timestampMs = nowMs;
  sample.valid = true;

  nextIndex_ = (nextIndex_ + 1) % sampleCapacity_;
  if (sampleCount_ < sampleCapacity_) {
    ++sampleCount_;
  }
  lastSampleMs_ = nowMs;
  lastSampleEastM_ = eastM;
  lastSampleNorthM_ = northM;
  lastSamplePositionValid_ = true;
}

void ThermalAssistant::updateParticles(float eastM, float northM, float liftMs, uint32_t nowMs, bool addNewSample) {
  for (uint16_t i = 0; i < particleCapacity_; ++i) {
    Particle& particle = particles_[i];
    if (!particle.valid) continue;

    const float dt = positiveAgeSeconds(nowMs, particle.timestampMs, 5.0F);
    if (dt > 0.0F) {
      particle.weight *= powf(kParticleDecayPerSecond, dt);
      particle.timestampMs = nowMs;
    }
    if (particle.weight < kParticleMinWeight) {
      particle = {};
    }
  }

  if (!addNewSample || !isThermalLift(liftMs)) {
    return;
  }

  auto upsertParticle = [&](float targetEast, float targetNorth, float weight, float lift) {
    uint16_t nearest = particleCapacity_;
    float nearestDistance = kParticleMergeDistanceM;
    uint16_t replacement = particleCapacity_;
    float weakestWeight = 9999.0F;

    for (uint16_t i = 0; i < particleCapacity_; ++i) {
      Particle& particle = particles_[i];
      if (!particle.valid) {
        if (replacement == particleCapacity_) {
          replacement = i;
        }
        continue;
      }

      const float d = distanceM(targetEast, targetNorth, particle.eastM, particle.northM);
      if (d < nearestDistance) {
        nearestDistance = d;
        nearest = i;
      }
      if (particle.weight < weakestWeight) {
        weakestWeight = particle.weight;
        replacement = i;
      }
    }

    const uint16_t index = nearest < particleCapacity_ ? nearest : replacement;
    if (index >= particleCapacity_) {
      return;
    }

    Particle& particle = particles_[index];
    if (particle.valid) {
      const float total = particle.weight + weight;
      const float blend = clampFloat(weight / fmaxf(total, 0.001F), 0.15F, 0.75F);
      particle.eastM += (targetEast - particle.eastM) * blend;
      particle.northM += (targetNorth - particle.northM) * blend;
      particle.weight = clampFloat(total, 0.0F, 8.0F);
      particle.liftMs = fmaxf(particle.liftMs * 0.92F, lift);
      particle.timestampMs = nowMs;
    } else {
      particle.eastM = targetEast;
      particle.northM = targetNorth;
      particle.weight = clampFloat(weight, 0.0F, 8.0F);
      particle.liftMs = lift;
      particle.timestampMs = nowMs;
      particle.valid = true;
    }
  };

  static constexpr float kSeedOffsets[][2] = {
      {0.0F, 0.0F}, {1.0F, 0.0F}, {-1.0F, 0.0F}, {0.0F, 1.0F},
      {0.0F, -1.0F}, {0.72F, 0.72F}, {-0.72F, 0.72F}, {0.72F, -0.72F},
  };
  const uint8_t seedCount = particleCapacity_ >= 32 ? 8 : 4;
  const float radius = clampFloat(20.0F - liftMs * 2.0F, 8.0F, 20.0F);
  const float baseWeight = clampFloat(0.28F + liftMs * 0.38F, 0.18F, 2.2F);
  const float limitedLift = clampFloat(liftMs, -1.0F, 8.0F);
  for (uint8_t i = 0; i < seedCount; ++i) {
    const float edgeScale = i == 0 ? 1.0F : 0.62F;
    upsertParticle(eastM + kSeedOffsets[i][0] * radius,
                   northM + kSeedOffsets[i][1] * radius,
                   baseWeight * edgeScale,
                   limitedLift);
  }
}

void ThermalAssistant::expireOldSamples(uint32_t nowMs) {
  uint16_t validCount = 0;
  for (uint16_t i = 0; i < sampleCapacity_; ++i) {
    Sample& sample = samples_[i];
    if (!sample.valid) continue;
    if (nowMs >= sample.timestampMs && nowMs - sample.timestampMs > kSampleMaxAgeMs) {
      sample.valid = false;
      continue;
    }
    ++validCount;
  }

  if (validCount == 0) {
    sampleCount_ = 0;
    nextIndex_ = 0;
    lastSamplePositionValid_ = false;
    driftValid_ = false;
    coreValid_ = false;
  } else {
    sampleCount_ = validCount;
  }
}

void ThermalAssistant::expireHistory(uint32_t nowMs) {
  for (uint8_t i = 0; i < kThermalHistoryPoints; ++i) {
    HistoryThermal& item = history_[i];
    if (!item.valid) continue;
    if (nowMs >= item.lastSeenMs && nowMs - item.lastSeenMs > kHistoryMaxAgeMs) {
      item = {};
      if (currentHistorySlot_ == i) {
        currentHistorySlot_ = 255;
      }
      continue;
    }
    if (nowMs >= item.lastSeenMs && nowMs - item.lastSeenMs > kLostThermalResetMs) {
      item.active = false;
      if (currentHistorySlot_ == i) {
        currentHistorySlot_ = 255;
      }
    }
  }
}

void ThermalAssistant::updateHistory(float centerEastM,
                                     float centerNorthM,
                                     float coreMs,
                                     uint8_t confidence,
                                     uint32_t nowMs,
                                     bool activeThermal) {
  if (!activeThermal || confidence < 22) {
    if (currentHistorySlot_ < kThermalHistoryPoints && history_[currentHistorySlot_].valid) {
      history_[currentHistorySlot_].active = false;
    }
    currentHistorySlot_ = 255;
    return;
  }

  uint8_t slot = 255;
  if (currentHistorySlot_ < kThermalHistoryPoints && history_[currentHistorySlot_].valid &&
      distanceM(centerEastM, centerNorthM, history_[currentHistorySlot_].eastM, history_[currentHistorySlot_].northM) <
          kHistoryMergeDistanceM * 1.7F) {
    slot = currentHistorySlot_;
  }

  if (slot == 255) {
    float nearestDistance = kHistoryMergeDistanceM;
    for (uint8_t i = 0; i < kThermalHistoryPoints; ++i) {
      if (!history_[i].valid) continue;
      const float d = distanceM(centerEastM, centerNorthM, history_[i].eastM, history_[i].northM);
      if (d < nearestDistance) {
        nearestDistance = d;
        slot = i;
      }
    }
  }

  if (slot == 255) {
    for (uint8_t i = 0; i < kThermalHistoryPoints; ++i) {
      if (!history_[i].valid) {
        slot = i;
        break;
      }
    }
  }

  if (slot == 255) {
    uint32_t oldestSeen = 0xFFFFFFFFUL;
    for (uint8_t i = 0; i < kThermalHistoryPoints; ++i) {
      if (history_[i].lastSeenMs < oldestSeen) {
        oldestSeen = history_[i].lastSeenMs;
        slot = i;
      }
    }
  }

  if (slot >= kThermalHistoryPoints) {
    return;
  }

  for (uint8_t i = 0; i < kThermalHistoryPoints; ++i) {
    history_[i].active = false;
  }

  HistoryThermal& item = history_[slot];
  if (!item.valid) {
    item.eastM = centerEastM;
    item.northM = centerNorthM;
    item.coreMs = coreMs;
    item.confidencePercent = confidence;
    item.firstSeenMs = nowMs;
    item.lastSeenMs = nowMs;
    item.active = true;
    item.valid = true;
  } else {
    const float alpha = clampFloat(0.18F + static_cast<float>(confidence) / 500.0F, 0.18F, 0.38F);
    item.eastM += (centerEastM - item.eastM) * alpha;
    item.northM += (centerNorthM - item.northM) * alpha;
    item.coreMs = fmaxf(item.coreMs * 0.96F, coreMs);
    const float blendedConfidence =
        static_cast<float>(item.confidencePercent) * 0.72F + static_cast<float>(confidence) * 0.28F;
    item.confidencePercent = clampPercent(fmaxf(blendedConfidence, static_cast<float>(confidence) * 0.86F));
    item.lastSeenMs = nowMs;
    item.active = true;
  }
  currentHistorySlot_ = slot;
}

void ThermalAssistant::populateHistoryOutput(VarioData& data, float originEastM, float originNorthM, uint32_t nowMs) const {
  bool used[kThermalHistoryPoints] = {};
  uint8_t outCount = 0;

  while (outCount < kThermalHistoryPoints) {
    uint8_t best = 255;
    for (uint8_t i = 0; i < kThermalHistoryPoints; ++i) {
      if (used[i] || !history_[i].valid) continue;
      if (best == 255) {
        best = i;
        continue;
      }
      if (history_[i].active != history_[best].active) {
        if (history_[i].active) {
          best = i;
        }
        continue;
      }
      if (history_[i].lastSeenMs > history_[best].lastSeenMs) {
        best = i;
      }
    }

    if (best == 255) {
      break;
    }

    used[best] = true;
    const HistoryThermal& item = history_[best];
    ThermalHistoryPoint& out = data.thermalHistory[outCount];
    out.eastM = item.eastM - originEastM;
    out.northM = item.northM - originNorthM;
    out.coreMs = item.coreMs;
    out.confidencePercent = item.confidencePercent;
    const uint32_t ageMinutes = nowMs >= item.lastSeenMs ? (nowMs - item.lastSeenMs) / 60000UL : 0UL;
    out.ageMinutes = static_cast<uint8_t>(ageMinutes > 255UL ? 255UL : ageMinutes);
    out.active = item.active;
    ++outCount;
  }

  for (uint8_t i = outCount; i < kThermalHistoryPoints; ++i) {
    data.thermalHistory[i] = {};
  }
  data.thermalHistoryCount = outCount;
}

void ThermalAssistant::populateOutput(VarioData& data,
                                      float currentEastM,
                                      float currentNorthM,
                                      bool currentPositionValid,
                                      bool isCircling) {
  const uint32_t nowMs = millis();
  if (sampleCount_ == 0) {
    data.thermalPointCount = 0;
    data.thermalRangeM = smoothedRangeM_;
    data.thermalDriftDeg = driftValid_ ? smoothedDriftDeg_ : normalizeDeg(data.courseDeg);
    data.thermalPilotEastM = 0.0F;
    data.thermalPilotNorthM = 0.0F;
    data.thermalCoreMs = 0.0F;
    data.thermalLockPercent = 0;
    data.thermalCoreConfidencePercent = 0;
    data.thermalDriftMode = ThermalAssistDriftMode::Classic;
    populateHistoryOutput(data, currentPositionValid ? currentEastM : 0.0F, currentPositionValid ? currentNorthM : 0.0F, nowMs);
    return;
  }

  float weightedEast = 0.0F;
  float weightedNorth = 0.0F;
  float weightSum = 0.0F;
  float geometricEast = 0.0F;
  float geometricNorth = 0.0F;
  float bestEast = 0.0F;
  float bestNorth = 0.0F;
  float bestLift = -999.0F;
  uint16_t validCount = 0;

  for (uint16_t i = 0; i < sampleCapacity_; ++i) {
    const Sample& sample = samples_[i];
    if (!sample.valid || !isThermalLift(sample.liftMs)) continue;
    const float weight = liftWeight(sample.liftMs);
    weightedEast += sample.eastM * weight;
    weightedNorth += sample.northM * weight;
    weightSum += weight;
    geometricEast += sample.eastM;
    geometricNorth += sample.northM;
    if (sample.liftMs > bestLift) {
      bestLift = sample.liftMs;
      bestEast = sample.eastM;
      bestNorth = sample.northM;
    }
    ++validCount;
  }

  if (validCount == 0 || weightSum <= 0.0F) {
    sampleCount_ = 0;
    data.thermalPointCount = 0;
    data.thermalLockPercent = 0;
    data.thermalCoreConfidencePercent = 0;
    data.thermalDriftMode = ThermalAssistDriftMode::Classic;
    populateHistoryOutput(data, currentPositionValid ? currentEastM : 0.0F, currentPositionValid ? currentNorthM : 0.0F, nowMs);
    return;
  }

  float centerEast = weightedEast / weightSum;
  float centerNorth = weightedNorth / weightSum;
  const float classicCenterEast = centerEast;
  const float classicCenterNorth = centerNorth;
  geometricEast /= static_cast<float>(validCount);
  geometricNorth /= static_cast<float>(validCount);

  float particleEast = 0.0F;
  float particleNorth = 0.0F;
  float particleWeightSum = 0.0F;
  uint16_t particleValidCount = 0;
  for (uint16_t i = 0; i < particleCapacity_; ++i) {
    const Particle& particle = particles_[i];
    if (!particle.valid || particle.weight <= kParticleMinWeight) continue;
    const float weight = particle.weight * liftWeight(particle.liftMs);
    particleEast += particle.eastM * weight;
    particleNorth += particle.northM * weight;
    particleWeightSum += weight;
    ++particleValidCount;
  }

  const bool particleCenterValid = particleValidCount >= 3 && particleWeightSum > 0.15F;
  float particleCenterEast = classicCenterEast;
  float particleCenterNorth = classicCenterNorth;
  if (particleCenterValid) {
    particleCenterEast = particleEast / particleWeightSum;
    particleCenterNorth = particleNorth / particleWeightSum;
  }

  float classicSpreadSum = 0.0F;
  for (uint16_t i = 0; i < sampleCapacity_; ++i) {
    const Sample& sample = samples_[i];
    if (!sample.valid || !isThermalLift(sample.liftMs)) continue;
    classicSpreadSum += distanceM(sample.eastM, sample.northM, classicCenterEast, classicCenterNorth);
  }
  const float classicAverageSpreadM = classicSpreadSum / static_cast<float>(validCount);
  const float particleDisagreementM =
      particleCenterValid ? distanceM(classicCenterEast, classicCenterNorth, particleCenterEast, particleCenterNorth) : 0.0F;
  const bool canUseAdvancedCore = psramReady_ && particleCenterValid && validCount >= 10 && particleValidCount >= 10 &&
                                  particleWeightSum > 1.0F && bestLift > 0.35F;
  const bool thermalLooksIrregular = classicAverageSpreadM > 42.0F || particleDisagreementM > 18.0F;
  const ThermalAssistDriftMode effectiveMode =
      canUseAdvancedCore && thermalLooksIrregular ? ThermalAssistDriftMode::Particle : ThermalAssistDriftMode::Classic;
  data.thermalDriftMode = effectiveMode;

  if (effectiveMode == ThermalAssistDriftMode::Particle) {
    centerEast = particleCenterEast;
    centerNorth = particleCenterNorth;
  }

  const float coreAlpha = effectiveMode == ThermalAssistDriftMode::Particle ? kCoreParticleAlpha : kCoreClassicAlpha;
  if (!coreValid_) {
    smoothedCoreEastM_ = centerEast;
    smoothedCoreNorthM_ = centerNorth;
    coreValid_ = true;
  } else {
    smoothedCoreEastM_ += (centerEast - smoothedCoreEastM_) * coreAlpha;
    smoothedCoreNorthM_ += (centerNorth - smoothedCoreNorthM_) * coreAlpha;
  }
  centerEast = smoothedCoreEastM_;
  centerNorth = smoothedCoreNorthM_;

  const bool pilotCentered = data.thermalVisualMode == ThermalAssistVisualMode::PilotCentered;
  const float originEast = pilotCentered && currentPositionValid ? currentEastM : centerEast;
  const float originNorth = pilotCentered && currentPositionValid ? currentNorthM : centerNorth;
  data.thermalPilotEastM = currentPositionValid ? currentEastM - originEast : 0.0F;
  data.thermalPilotNorthM = currentPositionValid ? currentNorthM - originNorth : 0.0F;

  float maxDistanceM = 0.0F;
  uint16_t selected[kMaxVisibleThermalPoints] = {};
  uint8_t selectedCount = 0;
  for (uint16_t back = 1; back <= sampleCapacity_; ++back) {
    const uint16_t index = (nextIndex_ + sampleCapacity_ - back) % sampleCapacity_;
    if (!samples_[index].valid || !isThermalLift(samples_[index].liftMs)) continue;

    int16_t overlappingSlot = -1;
    for (uint8_t slot = 0; slot < selectedCount; ++slot) {
      const Sample& visible = samples_[selected[slot]];
      if (distanceM(samples_[index].eastM, samples_[index].northM, visible.eastM, visible.northM) < kVisiblePointSpacingM) {
        overlappingSlot = slot;
        break;
      }
    }

    if (overlappingSlot >= 0) {
      const uint16_t visibleIndex = selected[overlappingSlot];
      if (samples_[index].liftMs >= samples_[visibleIndex].liftMs + kVisiblePointReplacementLiftMs) {
        selected[overlappingSlot] = index;
      }
      continue;
    }

    if (selectedCount < kMaxVisibleThermalPoints) {
      selected[selectedCount++] = index;
    }
  }

  uint8_t outIndex = 0;
  for (uint8_t i = 0; i < selectedCount; ++i) {
    const Sample& sample = samples_[selected[selectedCount - 1 - i]];
    const float eastRel = sample.eastM - originEast;
    const float northRel = sample.northM - originNorth;
    const float d = sqrtf(eastRel * eastRel + northRel * northRel);
    if (d > maxDistanceM) {
      maxDistanceM = d;
    }
    data.thermalPoints[outIndex].eastM = eastRel;
    data.thermalPoints[outIndex].northM = northRel;
    data.thermalPoints[outIndex].liftMs = sample.liftMs;
    ++outIndex;
  }
  data.thermalPointCount = outIndex;

  const float pilotDistance = sqrtf(data.thermalPilotEastM * data.thermalPilotEastM + data.thermalPilotNorthM * data.thermalPilotNorthM);
  if (pilotDistance > maxDistanceM) {
    maxDistanceM = pilotDistance;
  }

  const float targetRange = clampFloat(maxDistanceM * kRangeMargin, kMinRangeM, kMaxRangeM);
  smoothedRangeM_ += (targetRange - smoothedRangeM_) * kRangeAlpha;
  data.thermalRangeM = smoothedRangeM_;

  float coreVectorEast = 0.0F;
  float coreVectorNorth = 0.0F;
  if (pilotCentered && currentPositionValid) {
    coreVectorEast = centerEast - currentEastM;
    coreVectorNorth = centerNorth - currentNorthM;
    if (sqrtf(coreVectorEast * coreVectorEast + coreVectorNorth * coreVectorNorth) < kMinCoreVectorM) {
      coreVectorEast = bestEast - currentEastM;
      coreVectorNorth = bestNorth - currentNorthM;
    }
  } else {
    coreVectorEast = bestEast - geometricEast;
    coreVectorNorth = bestNorth - geometricNorth;
  }
  const float coreVectorDistance = sqrtf(coreVectorEast * coreVectorEast + coreVectorNorth * coreVectorNorth);
  if (coreVectorDistance >= kMinCoreVectorM) {
    const float targetDrift = bearingFromVector(coreVectorEast, coreVectorNorth);
    smoothedDriftDeg_ = driftValid_ ? blendAngle(smoothedDriftDeg_, targetDrift, kDriftAlpha) : targetDrift;
    driftValid_ = true;
  } else if (!driftValid_) {
    smoothedDriftDeg_ = normalizeDeg(data.courseDeg);
  }
  data.thermalDriftDeg = smoothedDriftDeg_;
  data.thermalCoreMs = bestLift > -900.0F ? bestLift : 0.0F;

  float spreadSum = 0.0F;
  for (uint16_t i = 0; i < sampleCapacity_; ++i) {
    const Sample& sample = samples_[i];
    if (!sample.valid || !isThermalLift(sample.liftMs)) continue;
    spreadSum += distanceM(sample.eastM, sample.northM, centerEast, centerNorth);
  }
  const float averageSpreadM = spreadSum / static_cast<float>(validCount);
  const float liftScore = clampFloat((bestLift + 0.2F) * 16.0F, 0.0F, 44.0F);
  const float countScore = clampFloat(static_cast<float>(validCount) * 3.0F, 0.0F, 30.0F);
  const float particleScore =
      effectiveMode == ThermalAssistDriftMode::Particle ? clampFloat(static_cast<float>(particleValidCount) * 0.55F, 0.0F, 18.0F) : 0.0F;
  const float recencyScore = nowMs >= lastSampleMs_ && nowMs - lastSampleMs_ < 5000UL ? 8.0F : 0.0F;
  const float circlingScore = isCircling ? 12.0F : 0.0F;
  const float spreadPenalty = clampFloat(averageSpreadM * 0.16F, 0.0F, 28.0F);
  const uint8_t confidence = clampPercent(liftScore + countScore + particleScore + recencyScore + circlingScore - spreadPenalty);
  data.thermalLockPercent = confidence;
  data.thermalCoreConfidencePercent = confidence;

  const bool activeThermal = (isCircling && confidence >= 25) || confidence >= 62;
  updateHistory(centerEast, centerNorth, data.thermalCoreMs, confidence, nowMs, activeThermal);
  populateHistoryOutput(data, originEast, originNorth, nowMs);
}
