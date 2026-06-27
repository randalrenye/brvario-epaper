#include "navigation/ThermalCycleBeta.h"

#include <math.h>
#include <string.h>

namespace {

float absFloat(float value) {
  return value < 0.0F ? -value : value;
}

float clampFloat(float value, float lo, float hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

uint8_t clampConfidence(int value) {
  if (value < 0) return 0;
  if (value > 100) return 100;
  return static_cast<uint8_t>(value);
}

int16_t clampMilliHpa(float valueHpa) {
  const float scaled = valueHpa * 1000.0F;
  if (scaled > 32767.0F) return 32767;
  if (scaled < -32768.0F) return -32768;
  return static_cast<int16_t>(scaled + (scaled >= 0.0F ? 0.5F : -0.5F));
}

bool periodsAgree(uint32_t aMs, uint32_t bMs) {
  if (aMs == 0 || bMs == 0) {
    return false;
  }
  const uint32_t big = aMs > bMs ? aMs : bMs;
  const uint32_t small = aMs > bMs ? bMs : aMs;
  return big - small <= big / 3UL;
}

}  // namespace

void ThermalCycleBeta::reset() {
  initialized_ = false;
  pressureValid_ = false;
  startedMs_ = 0;
  lastSampleMs_ = 0;
  windowStartMs_ = 0;
  lastEventMs_ = 0;
  artifactMaskUntilMs_ = 0;
  movementLikelyUntilMs_ = 0;
  lastCorrelationMs_ = 0;
  lastPulseCandidateMs_ = 0;
  sampleAccumulatorHpa_ = 0.0F;
  sampleAccumulatorCount_ = 0;
  processedSampleCount_ = 0;
  currentPressureHpa_ = 0.0F;
  previousPressureHpa_ = 0.0F;
  shortEmaHpa_ = 0.0F;
  longEmaHpa_ = 0.0F;
  residualHpa_ = 0.0F;
  previousResidualHpa_ = 0.0F;
  derivativeEmaHpaPerSec_ = 0.0F;
  noiseHpa_ = 0.0F;
  memset(history_, 0, sizeof(history_));
  historyWriteIndex_ = 0;
  historyCount_ = 0;
  bestPeriodMs_ = 0;
  bestCorrelation_ = 0.0F;
  periodScorePercent_ = 0;
  resetPulse();
  memset(events_, 0, sizeof(events_));
  eventWriteIndex_ = 0;
  eventCount_ = 0;
}

void ThermalCycleBeta::markArtifact(uint32_t nowMs, uint32_t durationMs) {
  const uint32_t requestedUntil = nowMs + durationMs;
  if (artifactMaskUntilMs_ == 0 || static_cast<int32_t>(requestedUntil - artifactMaskUntilMs_) > 0) {
    artifactMaskUntilMs_ = requestedUntil;
  }
  resetPulse();
}

bool ThermalCycleBeta::ingestPressureHpa(float pressureHpa, bool valid, uint32_t nowMs) {
  if (!valid || !isfinite(pressureHpa) || pressureHpa < 200.0F || pressureHpa > 1200.0F) {
    pressureValid_ = false;
    return false;
  }

  pressureValid_ = true;
  if (windowStartMs_ == 0) {
    windowStartMs_ = nowMs;
  }

  sampleAccumulatorHpa_ += pressureHpa;
  ++sampleAccumulatorCount_;

  if (nowMs - windowStartMs_ < THERMAL_SAMPLE_PERIOD_MS) {
    return false;
  }

  const float averageHpa = sampleAccumulatorHpa_ / static_cast<float>(sampleAccumulatorCount_);
  sampleAccumulatorHpa_ = 0.0F;
  sampleAccumulatorCount_ = 0;
  windowStartMs_ = nowMs;
  return processOneSecondSample(averageHpa, nowMs);
}

ThermalCycleBeta::Snapshot ThermalCycleBeta::snapshot(uint32_t nowMs) const {
  Snapshot out;
  out.pressureValid = pressureValid_ && initialized_;
  out.elapsedMs = elapsedSinceStart(nowMs);
  out.warmupRemainingMs = out.elapsedMs >= THERMAL_WARMUP_MS ? 0 : THERMAL_WARMUP_MS - out.elapsedMs;
  out.sampleCount = processedSampleCount_;
  out.eventCount = eventCount_;
  out.pressureHpa = currentPressureHpa_;
  out.shortEmaHpa = shortEmaHpa_;
  out.longEmaHpa = longEmaHpa_;
  out.residualHpa = residualHpa_;
  out.dPdtHpaPerSec = derivativeEmaHpaPerSec_;
  out.noiseHpa = noiseHpa_;
  out.lastEventIntensity = eventCount_ > 0 ? events_[(eventWriteIndex_ + kMaxEvents - 1) % kMaxEvents].intensity : 0.0F;
  out.lastEventAgeMs = lastEventMs_ == 0 ? 0 : nowMs - lastEventMs_;
  out.bestPeriodMs = bestPeriodMs_;
  out.periodScorePercent = periodScorePercent_;
  out.qualityPercent = computeQuality(nowMs);
  out.patternStable = bestPeriodMs_ >= MIN_CYCLE_MS && periodScorePercent_ >= 45;
  out.movementLikely = movementLikelyUntilMs_ != 0 && static_cast<int32_t>(movementLikelyUntilMs_ - nowMs) > 0;
  out.artifactMasked = artifactMaskUntilMs_ != 0 && static_cast<int32_t>(artifactMaskUntilMs_ - nowMs) > 0;
  out.pulseCandidate = pulseActive_ || (lastPulseCandidateMs_ != 0 && nowMs - lastPulseCandidateMs_ <= kPossiblePulseVisibleMs);
  out.pulseCandidateAgeMs = lastPulseCandidateMs_ == 0 ? 0 : nowMs - lastPulseCandidateMs_;

  uint32_t intervals[kMaxEvents - 1] = {};
  out.intervalCount = collectIntervals(intervals, static_cast<uint8_t>(kMaxEvents - 1));
  if (out.intervalCount > 0) {
    out.medianCycleMs = medianInterval(intervals, out.intervalCount);
    out.averageCycleMs = averageInterval(intervals, out.intervalCount);
  }

  const float variation = intervalVariation(intervals, out.intervalCount, out.medianCycleMs);
  out.confidencePercent =
      computeConfidence(nowMs, out.medianCycleMs, variation, out.intervalCount, out.bestPeriodMs, out.periodScorePercent, out.qualityPercent);

  const bool eventCycle = eventCount_ >= 3 && out.intervalCount >= 2 && out.medianCycleMs >= MIN_CYCLE_MS;
  const bool autocorrCycle = out.patternStable && eventCount_ >= 2;
  out.hasCycle = eventCycle || autocorrCycle;
  if (eventCycle && autocorrCycle && periodsAgree(out.medianCycleMs, out.bestPeriodMs)) {
    out.detectedCycleMs = (out.medianCycleMs + out.bestPeriodMs) / 2UL;
  } else if (eventCycle) {
    out.detectedCycleMs = out.medianCycleMs;
  } else if (autocorrCycle) {
    out.detectedCycleMs = out.bestPeriodMs;
  }

  if (out.hasCycle && lastEventMs_ != 0 && out.detectedCycleMs >= MIN_CYCLE_MS) {
    const uint32_t targetMs = lastEventMs_ + out.detectedCycleMs;
    out.nextCycleRemainingMs = static_cast<int32_t>(targetMs - nowMs);
    out.cycleMissed = out.nextCycleRemainingMs < -static_cast<int32_t>(MISSED_WINDOW_MS);
  }
  if (out.cycleMissed && out.confidencePercent > 35) {
    out.confidencePercent = 35;
  } else if (out.nextCycleRemainingMs < -static_cast<int32_t>(NEAR_WINDOW_AFTER_MS) && out.confidencePercent > 45) {
    out.confidencePercent = static_cast<uint8_t>(out.confidencePercent - 10);
  }
  out.hasPrediction = out.hasCycle && !out.cycleMissed && out.confidencePercent >= 40 && out.qualityPercent >= 35 && lastEventMs_ != 0 &&
                      out.detectedCycleMs >= MIN_CYCLE_MS;
  if (out.hasPrediction) {
    const uint32_t targetMs = lastEventMs_ + out.detectedCycleMs;
    out.nextCycleRemainingMs = static_cast<int32_t>(targetMs - nowMs);
  }

  if (!out.pressureValid) {
    out.status = Status::InsufficientSignal;
  } else if (!warmupComplete(nowMs)) {
    out.status = Status::CollectingBase;
  } else if (out.artifactMasked || out.movementLikely) {
    out.status = Status::Observing;
  } else if (out.pulseCandidate) {
    out.status = Status::PossiblePulse;
  } else if (noiseHpa_ > kMaxNoisyPressureHpa && eventCount_ < 3) {
    out.status = Status::InsufficientSignal;
  } else if (eventCount_ >= 3 && out.intervalCount >= 2 && variation > 0.55F) {
    out.status = Status::IrregularCycle;
  } else if (out.cycleMissed) {
    out.status = Status::AwaitingNewPulse;
  } else if (out.hasPrediction && out.nextCycleRemainingMs <= static_cast<int32_t>(NEAR_WINDOW_BEFORE_MS) &&
             out.nextCycleRemainingMs >= -static_cast<int32_t>(NEAR_WINDOW_AFTER_MS)) {
    out.status = Status::ProbableWindow;
  } else if (out.hasPrediction && out.nextCycleRemainingMs > static_cast<int32_t>(NEAR_WINDOW_BEFORE_MS)) {
    out.status = Status::BetweenCycles;
  } else if (out.hasCycle && out.confidencePercent >= 40 && (variation <= 0.55F || out.patternStable)) {
    out.status = Status::ProbableCycle;
  } else if (eventCount_ >= 3 || out.periodScorePercent >= 25) {
    out.status = Status::IrregularCycle;
  } else {
    out.status = Status::Observing;
  }
  return out;
}

const char* ThermalCycleBeta::statusLabel(Status status) {
  switch (status) {
    case Status::CollectingBase:
      return "COLETANDO BASE";
    case Status::Observing:
      return "OBSERVANDO";
    case Status::PossiblePulse:
      return "POSSIVEL PULSO";
    case Status::BetweenCycles:
      return "ENTRE CICLOS";
    case Status::ProbableWindow:
      return "JANELA PROVAVEL";
    case Status::ProbableCycle:
      return "CICLO PROVAVEL";
    case Status::IrregularCycle:
      return "CICLO IRREGULAR";
    case Status::AwaitingNewPulse:
      return "AGUARDANDO NOVO PULSO";
    case Status::InsufficientSignal:
    default:
      return "SEM SINAL SUFICIENTE";
  }
}

const char* ThermalCycleBeta::confidenceLabel(uint8_t confidencePercent) {
  if (confidencePercent <= 30) return "BAIXA";
  if (confidencePercent <= 65) return "MEDIA";
  return "ALTA";
}

const char* ThermalCycleBeta::noiseLabel(float noiseHpa) {
  if (noiseHpa < 0.025F) return "baixo";
  if (noiseHpa < 0.080F) return "medio";
  return "alto";
}

bool ThermalCycleBeta::processOneSecondSample(float pressureHpa, uint32_t nowMs) {
  if (!initialized_) {
    initialized_ = true;
    startedMs_ = nowMs;
    lastSampleMs_ = nowMs;
    currentPressureHpa_ = pressureHpa;
    previousPressureHpa_ = pressureHpa;
    shortEmaHpa_ = pressureHpa;
    longEmaHpa_ = pressureHpa;
    residualHpa_ = 0.0F;
    previousResidualHpa_ = 0.0F;
    derivativeEmaHpaPerSec_ = 0.0F;
    noiseHpa_ = kNoiseFloorHpa;
    processedSampleCount_ = 1;
    appendHistory(nowMs, 0);
    return true;
  }

  uint32_t dtMs = nowMs - lastSampleMs_;
  if (dtMs == 0) {
    dtMs = THERMAL_SAMPLE_PERIOD_MS;
  }
  lastSampleMs_ = nowMs;
  if (processedSampleCount_ < UINT16_MAX) {
    ++processedSampleCount_;
  }

  const float pressureStepHpa = absFloat(pressureHpa - previousPressureHpa_);
  previousPressureHpa_ = pressureHpa;
  currentPressureHpa_ = pressureHpa;
  if (pressureStepHpa >= kMovementStepHpa) {
    movementLikelyUntilMs_ = nowMs + kMovementMaskMs;
    markArtifact(nowMs, kMovementMaskMs);
  }

  const float shortAlpha = emaAlpha(dtMs, kShortEmaTauMs);
  const float longAlpha = emaAlpha(dtMs, kLongEmaTauMs);
  shortEmaHpa_ += shortAlpha * (pressureHpa - shortEmaHpa_);
  longEmaHpa_ += longAlpha * (pressureHpa - longEmaHpa_);

  previousResidualHpa_ = residualHpa_;
  residualHpa_ = shortEmaHpa_ - longEmaHpa_;
  const float dtSeconds = static_cast<float>(dtMs) / 1000.0F;
  const float rawDerivative = dtSeconds > 0.0F ? (residualHpa_ - previousResidualHpa_) / dtSeconds : 0.0F;
  derivativeEmaHpaPerSec_ += emaAlpha(dtMs, kDerivativeTauMs) * (rawDerivative - derivativeEmaHpaPerSec_);

  const float residualStep = absFloat(residualHpa_ - previousResidualHpa_);
  noiseHpa_ += emaAlpha(dtMs, kNoiseTauMs) * (residualStep - noiseHpa_);
  if (noiseHpa_ < kNoiseFloorHpa) {
    noiseHpa_ = kNoiseFloorHpa;
  }

  uint8_t sampleFlags = 0;
  if (artifactMaskUntilMs_ != 0 && static_cast<int32_t>(artifactMaskUntilMs_ - nowMs) > 0) {
    sampleFlags |= kHistoryFlagArtifact;
  }
  if (movementLikelyUntilMs_ != 0 && static_cast<int32_t>(movementLikelyUntilMs_ - nowMs) > 0) {
    sampleFlags |= kHistoryFlagMovement;
  }
  if (noiseHpa_ > kMaxNoisyPressureHpa) {
    sampleFlags |= kHistoryFlagNoisy;
  }
  appendHistory(nowMs, sampleFlags);
  if (warmupComplete(nowMs) && (lastCorrelationMs_ == 0 || nowMs - lastCorrelationMs_ >= kCorrelationUpdateMs)) {
    updateCorrelation(nowMs);
  }

  if (!warmupComplete(nowMs) || noiseHpa_ > kMaxNoisyPressureHpa || sampleFlags != 0) {
    resetPulse();
    return true;
  }

  const float safeNoise = noiseHpa_ > kNoiseFloorHpa ? noiseHpa_ : kNoiseFloorHpa;
  const float residualAbs = absFloat(residualHpa_);
  const float derivativeAbs = absFloat(derivativeEmaHpaPerSec_);
  const float sigmaThreshold = safeNoise * kEventSigmaFactor;
  const float eventThreshold = sigmaThreshold > kMinEventResidualHpa ? sigmaThreshold : kMinEventResidualHpa;
  const bool residualStrong = residualAbs >= eventThreshold;
  const bool derivativeStrong = derivativeAbs >= kMinEventDPdtHpaPerSec;
  const bool candidate = residualStrong || (residualAbs >= eventThreshold * 0.75F && derivativeStrong);
  if (!candidate) {
    updatePulseDetector(nowMs, dtMs, eventThreshold, safeNoise);
    return true;
  }

  updatePulseDetector(nowMs, dtMs, eventThreshold, safeNoise);
  return true;
}

void ThermalCycleBeta::updatePulseDetector(uint32_t nowMs, uint32_t dtMs, float eventThreshold, float safeNoise) {
  const float residualAbs = absFloat(residualHpa_);
  const float enterThreshold = eventThreshold * kPulseEnterFactor;
  const float releaseThreshold = eventThreshold * kPulseReleaseFactor;
  const float dtSeconds = static_cast<float>(dtMs) / 1000.0F;

  if (!pulseActive_) {
    if (residualAbs >= enterThreshold) {
      pulseActive_ = true;
      pulseStartMs_ = nowMs;
      pulseLastMs_ = nowMs;
      pulsePeakMs_ = nowMs;
      lastPulseCandidateMs_ = nowMs;
      pulsePeakAbsHpa_ = residualAbs;
      pulsePeakResidualHpa_ = residualHpa_;
      pulsePeakNoiseHpa_ = safeNoise;
      pulseAreaHpaSec_ = residualAbs * dtSeconds;
      pulseDerivativePeakHpaPerSec_ = absFloat(derivativeEmaHpaPerSec_);
    }
    return;
  }

  pulseAreaHpaSec_ += residualAbs * dtSeconds;
  pulseLastMs_ = nowMs;
  lastPulseCandidateMs_ = nowMs;
  if (residualAbs > pulsePeakAbsHpa_) {
    pulsePeakAbsHpa_ = residualAbs;
    pulsePeakResidualHpa_ = residualHpa_;
    pulsePeakNoiseHpa_ = safeNoise;
    pulsePeakMs_ = nowMs;
  }
  const float derivativeAbs = absFloat(derivativeEmaHpaPerSec_);
  if (derivativeAbs > pulseDerivativePeakHpaPerSec_) {
    pulseDerivativePeakHpaPerSec_ = derivativeAbs;
  }

  const uint32_t durationMs = nowMs - pulseStartMs_;
  if (durationMs > kMaxPulseDurationMs) {
    resetPulse();
    return;
  }
  if (residualAbs <= releaseThreshold && durationMs >= kMinPulseDurationMs) {
    finishPulse(nowMs, eventThreshold, safeNoise);
  }
}

void ThermalCycleBeta::resetPulse() {
  pulseActive_ = false;
  pulseStartMs_ = 0;
  pulseLastMs_ = 0;
  pulsePeakMs_ = 0;
  pulsePeakAbsHpa_ = 0.0F;
  pulsePeakResidualHpa_ = 0.0F;
  pulsePeakNoiseHpa_ = 0.0F;
  pulseAreaHpaSec_ = 0.0F;
  pulseDerivativePeakHpaPerSec_ = 0.0F;
}

void ThermalCycleBeta::finishPulse(uint32_t nowMs, float eventThreshold, float safeNoise) {
  const uint32_t durationMs = nowMs - pulseStartMs_;
  const bool durationOk = durationMs >= kMinPulseDurationMs && durationMs <= kMaxPulseDurationMs;
  const bool peakOk = pulsePeakAbsHpa_ >= eventThreshold;
  const bool areaOk = pulseAreaHpaSec_ >= kMinPulseAreaHpaSec;
  const bool derivativeOk = pulseDerivativePeakHpaPerSec_ >= kMinEventDPdtHpaPerSec;
  if (!durationOk || !peakOk || !areaOk || !derivativeOk) {
    resetPulse();
    return;
  }
  if (lastEventMs_ != 0 && pulsePeakMs_ - lastEventMs_ < MIN_EVENT_GAP_MS) {
    resetPulse();
    return;
  }

  int shapeScore = 20;
  const float intensity = clampFloat(pulsePeakAbsHpa_ / (pulsePeakNoiseHpa_ > kNoiseFloorHpa ? pulsePeakNoiseHpa_ : safeNoise), 0.0F, 99.0F);
  if (intensity >= 3.0F) {
    shapeScore += 25;
  } else if (intensity >= 2.0F) {
    shapeScore += 14;
  }
  if (durationMs >= 15000UL && durationMs <= 150000UL) {
    shapeScore += 25;
  } else if (durationMs <= kMaxPulseDurationMs) {
    shapeScore += 12;
  }
  if (pulseAreaHpaSec_ >= 0.35F) {
    shapeScore += 20;
  } else if (pulseAreaHpaSec_ >= kMinPulseAreaHpaSec) {
    shapeScore += 10;
  }
  if (pulseDerivativePeakHpaPerSec_ >= kMinEventDPdtHpaPerSec * 2.0F) {
    shapeScore += 10;
  }

  registerEvent(pulsePeakMs_, intensity, clampConfidence(shapeScore));
  resetPulse();
}

void ThermalCycleBeta::registerEvent(uint32_t eventMs, float intensity, uint8_t shapeScorePercent) {
  const uint32_t previousEventMs = lastEventMs_;
  Event& event = events_[eventWriteIndex_];
  event.timestampMs = eventMs;
  event.intensity = intensity;
  event.shapeScorePercent = shapeScorePercent;
  event.residualHpa = pulsePeakResidualHpa_ != 0.0F ? pulsePeakResidualHpa_ : residualHpa_;
  event.noiseHpa = pulsePeakNoiseHpa_ > 0.0F ? pulsePeakNoiseHpa_ : noiseHpa_;

  eventWriteIndex_ = static_cast<uint8_t>((eventWriteIndex_ + 1) % kMaxEvents);
  if (eventCount_ < kMaxEvents) {
    ++eventCount_;
  }
  lastEventMs_ = eventMs;

  uint32_t intervalMs = 0;
  if (previousEventMs != 0) {
    intervalMs = eventMs - previousEventMs;
  }
  const Snapshot snap = snapshot(eventMs);
  Serial.printf("[THERMAL_BETA] Evento de pressao: tempo=%lu residual=%.4f noise=%.4f intervalo=%lu shape=%u confidence=%u\n",
                static_cast<unsigned long>(eventMs),
                static_cast<double>(residualHpa_),
                static_cast<double>(noiseHpa_),
                static_cast<unsigned long>(intervalMs),
                static_cast<unsigned>(shapeScorePercent),
                static_cast<unsigned>(snap.confidencePercent));
}

void ThermalCycleBeta::appendHistory(uint32_t nowMs, uint8_t flags) {
  (void)nowMs;
  HistorySample& sample = history_[historyWriteIndex_];
  sample.residualMilliHpa = clampMilliHpa(residualHpa_);
  sample.flags = flags;
  historyWriteIndex_ = static_cast<uint16_t>((historyWriteIndex_ + 1U) % kHistorySamples);
  if (historyCount_ < kHistorySamples) {
    ++historyCount_;
  }
}

bool ThermalCycleBeta::historySampleFromLatest(uint16_t offset, HistorySample& sample) const {
  if (offset >= historyCount_) {
    return false;
  }
  const uint16_t latest = historyWriteIndex_ == 0 ? static_cast<uint16_t>(kHistorySamples - 1U) : static_cast<uint16_t>(historyWriteIndex_ - 1U);
  const uint16_t index = latest >= offset ? static_cast<uint16_t>(latest - offset)
                                          : static_cast<uint16_t>(kHistorySamples + latest - offset);
  sample = history_[index];
  return true;
}

void ThermalCycleBeta::updateCorrelation(uint32_t nowMs) {
  lastCorrelationMs_ = nowMs;
  bestPeriodMs_ = 0;
  bestCorrelation_ = 0.0F;
  periodScorePercent_ = 0;
  if (historyCount_ < kCorrelationMinLagSamples * 2U) {
    return;
  }

  const uint16_t maxLag = historyCount_ > kCorrelationMaxLagSamples ? kCorrelationMaxLagSamples : static_cast<uint16_t>(historyCount_ / 2U);
  for (uint16_t lag = kCorrelationMinLagSamples; lag <= maxLag; lag = static_cast<uint16_t>(lag + kCorrelationLagStepSamples)) {
    const uint16_t availablePairs = static_cast<uint16_t>(historyCount_ - lag);
    const uint16_t windowPairs = availablePairs > kCorrelationWindowSamples ? kCorrelationWindowSamples : availablePairs;
    float sumX = 0.0F;
    float sumY = 0.0F;
    uint16_t count = 0;
    for (uint16_t offset = 0; offset < windowPairs; offset = static_cast<uint16_t>(offset + kCorrelationStrideSamples)) {
      HistorySample x;
      HistorySample y;
      if (!historySampleFromLatest(offset, x) || !historySampleFromLatest(static_cast<uint16_t>(offset + lag), y)) {
        continue;
      }
      if ((x.flags | y.flags) != 0) {
        continue;
      }
      sumX += static_cast<float>(x.residualMilliHpa);
      sumY += static_cast<float>(y.residualMilliHpa);
      ++count;
    }
    if (count < kCorrelationMinPairs) {
      continue;
    }

    const float meanX = sumX / static_cast<float>(count);
    const float meanY = sumY / static_cast<float>(count);
    float covariance = 0.0F;
    float energyX = 0.0F;
    float energyY = 0.0F;
    for (uint16_t offset = 0; offset < windowPairs; offset = static_cast<uint16_t>(offset + kCorrelationStrideSamples)) {
      HistorySample x;
      HistorySample y;
      if (!historySampleFromLatest(offset, x) || !historySampleFromLatest(static_cast<uint16_t>(offset + lag), y)) {
        continue;
      }
      if ((x.flags | y.flags) != 0) {
        continue;
      }
      const float dx = static_cast<float>(x.residualMilliHpa) - meanX;
      const float dy = static_cast<float>(y.residualMilliHpa) - meanY;
      covariance += dx * dy;
      energyX += dx * dx;
      energyY += dy * dy;
    }
    if (energyX < 1.0F || energyY < 1.0F) {
      continue;
    }
    const float correlation = covariance / sqrtf(energyX * energyY);
    if (correlation > bestCorrelation_) {
      bestCorrelation_ = correlation;
      bestPeriodMs_ = static_cast<uint32_t>(lag) * THERMAL_SAMPLE_PERIOD_MS;
    }
  }

  if (bestCorrelation_ <= 0.18F || bestPeriodMs_ == 0) {
    return;
  }
  const float normalized = (bestCorrelation_ - 0.18F) / 0.42F;
  periodScorePercent_ = clampConfidence(static_cast<int>(normalized * 100.0F));
}

uint8_t ThermalCycleBeta::collectIntervals(uint32_t* intervalsMs, uint8_t capacity) const {
  if (!intervalsMs || capacity == 0 || eventCount_ < 2) {
    return 0;
  }

  Event ordered[kMaxEvents] = {};
  const uint8_t count = chronologicalEvents(ordered, kMaxEvents);
  uint8_t written = 0;
  for (uint8_t i = 1; i < count && written < capacity; ++i) {
    const uint32_t interval = ordered[i].timestampMs - ordered[i - 1].timestampMs;
    if (interval >= MIN_CYCLE_MS && interval <= MAX_CYCLE_MS) {
      intervalsMs[written++] = interval;
    }
  }
  return written;
}

uint8_t ThermalCycleBeta::chronologicalEvents(Event* events, uint8_t capacity) const {
  if (!events || capacity == 0 || eventCount_ == 0) {
    return 0;
  }

  const uint8_t count = eventCount_ < capacity ? eventCount_ : capacity;
  const uint8_t first = eventCount_ == kMaxEvents ? eventWriteIndex_ : 0;
  for (uint8_t i = 0; i < count; ++i) {
    events[i] = events_[(first + i) % kMaxEvents];
  }
  return count;
}

uint32_t ThermalCycleBeta::medianInterval(uint32_t* intervalsMs, uint8_t count) const {
  if (!intervalsMs || count == 0) {
    return 0;
  }

  for (uint8_t i = 1; i < count; ++i) {
    const uint32_t key = intervalsMs[i];
    int8_t j = static_cast<int8_t>(i) - 1;
    while (j >= 0 && intervalsMs[j] > key) {
      intervalsMs[j + 1] = intervalsMs[j];
      --j;
    }
    intervalsMs[j + 1] = key;
  }

  if ((count & 1U) == 1U) {
    return intervalsMs[count / 2];
  }
  return (intervalsMs[count / 2 - 1] + intervalsMs[count / 2]) / 2UL;
}

uint32_t ThermalCycleBeta::averageInterval(const uint32_t* intervalsMs, uint8_t count) const {
  if (!intervalsMs || count == 0) {
    return 0;
  }
  uint64_t total = 0;
  for (uint8_t i = 0; i < count; ++i) {
    total += intervalsMs[i];
  }
  return static_cast<uint32_t>(total / count);
}

float ThermalCycleBeta::intervalVariation(const uint32_t* intervalsMs, uint8_t count, uint32_t medianMs) const {
  if (!intervalsMs || count < 2 || medianMs == 0) {
    return 1.0F;
  }

  float deviationTotal = 0.0F;
  for (uint8_t i = 0; i < count; ++i) {
    const int32_t delta = static_cast<int32_t>(intervalsMs[i]) - static_cast<int32_t>(medianMs);
    deviationTotal += absFloat(static_cast<float>(delta));
  }
  const float meanDeviation = deviationTotal / static_cast<float>(count);
  return meanDeviation / static_cast<float>(medianMs);
}

uint8_t ThermalCycleBeta::computeQuality(uint32_t nowMs) const {
  if (!pressureValid_ || !initialized_) {
    return 0;
  }
  int quality = 100;
  if (!warmupComplete(nowMs)) {
    const uint32_t elapsed = elapsedSinceStart(nowMs);
    quality = static_cast<int>((static_cast<uint64_t>(elapsed) * 60ULL) / THERMAL_WARMUP_MS);
  }
  if (noiseHpa_ > 0.20F) {
    quality -= 35;
  } else if (noiseHpa_ > 0.08F) {
    quality -= 22;
  } else if (noiseHpa_ > 0.025F) {
    quality -= 10;
  }
  if (artifactMaskUntilMs_ != 0 && static_cast<int32_t>(artifactMaskUntilMs_ - nowMs) > 0) {
    quality -= 35;
  }
  if (movementLikelyUntilMs_ != 0 && static_cast<int32_t>(movementLikelyUntilMs_ - nowMs) > 0) {
    quality -= 45;
  }
  if (historyCount_ < kCorrelationMinLagSamples) {
    quality -= 10;
  }
  return clampConfidence(quality);
}

uint8_t ThermalCycleBeta::computeConfidence(uint32_t nowMs,
                                            uint32_t medianMs,
                                            float variation,
                                            uint8_t intervalCount,
                                            uint32_t bestPeriodMs,
                                            uint8_t periodScorePercent,
                                            uint8_t qualityPercent) const {
  int confidence = 0;
  if (eventCount_ >= 3 && intervalCount >= 2) confidence += 30;
  if (eventCount_ >= 5) confidence += 15;

  if (variation <= 0.20F) {
    confidence += 30;
  } else if (variation <= 0.35F) {
    confidence += 18;
  } else if (variation <= 0.55F) {
    confidence += 8;
  }

  Event ordered[kMaxEvents] = {};
  const uint8_t count = chronologicalEvents(ordered, kMaxEvents);
  float intensityTotal = 0.0F;
  uint16_t shapeTotal = 0;
  for (uint8_t i = 0; i < count; ++i) {
    intensityTotal += ordered[i].intensity;
    shapeTotal += ordered[i].shapeScorePercent;
  }
  const float averageIntensity = count == 0 ? 0.0F : intensityTotal / static_cast<float>(count);
  const uint8_t averageShape = count == 0 ? 0 : static_cast<uint8_t>(shapeTotal / count);
  if (averageIntensity >= 3.0F) {
    confidence += 15;
  } else if (averageIntensity >= 2.0F) {
    confidence += 8;
  }
  if (averageShape >= 70) {
    confidence += 12;
  } else if (averageShape >= 50) {
    confidence += 6;
  }

  if (noiseHpa_ < 0.025F) {
    confidence += 15;
  } else if (noiseHpa_ < 0.080F) {
    confidence += 8;
  }

  if (periodScorePercent >= 70) {
    confidence += 25;
  } else if (periodScorePercent >= 45) {
    confidence += 16;
  } else if (periodScorePercent >= 25) {
    confidence += 8;
  }
  if (periodsAgree(medianMs, bestPeriodMs)) {
    confidence += 15;
  } else if (medianMs != 0 && bestPeriodMs != 0 && eventCount_ >= 3) {
    confidence -= 10;
  }

  if (lastEventMs_ != 0 && medianMs != 0 && nowMs - lastEventMs_ <= medianMs + medianMs / 2UL) {
    confidence += 10;
  } else if (lastEventMs_ != 0 && bestPeriodMs != 0 && nowMs - lastEventMs_ <= bestPeriodMs + bestPeriodMs / 2UL) {
    confidence += 8;
  }

  if (qualityPercent >= 80) {
    confidence += 8;
  } else if (qualityPercent < 35) {
    confidence = confidence > 25 ? 25 : confidence;
  }

  if (elapsedSinceStart(nowMs) < THERMAL_WARMUP_MS) {
    confidence = 0;
  }
  if (noiseHpa_ > kMaxNoisyPressureHpa) {
    confidence = confidence > 30 ? 30 : confidence;
  }
  if (artifactMaskUntilMs_ != 0 && static_cast<int32_t>(artifactMaskUntilMs_ - nowMs) > 0) {
    confidence = confidence > 25 ? 25 : confidence;
  }
  if (movementLikelyUntilMs_ != 0 && static_cast<int32_t>(movementLikelyUntilMs_ - nowMs) > 0) {
    confidence = confidence > 20 ? 20 : confidence;
  }
  return clampConfidence(confidence);
}

float ThermalCycleBeta::emaAlpha(uint32_t dtMs, uint32_t tauMs) const {
  if (tauMs == 0) {
    return 1.0F;
  }
  const float ratio = static_cast<float>(dtMs) / static_cast<float>(tauMs);
  return clampFloat(1.0F - expf(-ratio), 0.0F, 1.0F);
}

bool ThermalCycleBeta::warmupComplete(uint32_t nowMs) const {
  return initialized_ && elapsedSinceStart(nowMs) >= THERMAL_WARMUP_MS;
}

uint32_t ThermalCycleBeta::elapsedSinceStart(uint32_t nowMs) const {
  if (!initialized_ || startedMs_ == 0) {
    return 0;
  }
  return nowMs - startedMs_;
}
