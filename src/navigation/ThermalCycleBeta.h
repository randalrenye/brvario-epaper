#pragma once

#include <Arduino.h>
#include <stdint.h>

class ThermalCycleBeta {
 public:
  enum class Status : uint8_t {
    CollectingBase,
    Observing,
    PossiblePulse,
    BetweenCycles,
    ProbableWindow,
    ProbableCycle,
    IrregularCycle,
    AwaitingNewPulse,
    InsufficientSignal,
  };

  struct Snapshot {
    Status status = Status::CollectingBase;
    bool pressureValid = false;
    bool hasPrediction = false;
    bool hasCycle = false;
    bool patternStable = false;
    bool movementLikely = false;
    bool artifactMasked = false;
    bool pulseCandidate = false;
    bool cycleMissed = false;
    uint32_t elapsedMs = 0;
    uint32_t warmupRemainingMs = 0;
    uint32_t lastEventAgeMs = 0;
    uint32_t pulseCandidateAgeMs = 0;
    uint32_t medianCycleMs = 0;
    uint32_t averageCycleMs = 0;
    uint32_t bestPeriodMs = 0;
    uint32_t detectedCycleMs = 0;
    int32_t nextCycleRemainingMs = 0;
    uint16_t sampleCount = 0;
    uint8_t eventCount = 0;
    uint8_t intervalCount = 0;
    uint8_t confidencePercent = 0;
    uint8_t qualityPercent = 0;
    uint8_t periodScorePercent = 0;
    float pressureHpa = 0.0F;
    float shortEmaHpa = 0.0F;
    float longEmaHpa = 0.0F;
    float residualHpa = 0.0F;
    float dPdtHpaPerSec = 0.0F;
    float noiseHpa = 0.0F;
    float lastEventIntensity = 0.0F;
  };

  static constexpr uint32_t THERMAL_SAMPLE_PERIOD_MS = 1000UL;
  static constexpr uint32_t THERMAL_WARMUP_MS = 5UL * 60UL * 1000UL;
  static constexpr uint32_t MIN_EVENT_GAP_MS = 3UL * 60UL * 1000UL;
  static constexpr uint32_t MIN_CYCLE_MS = 3UL * 60UL * 1000UL;
  static constexpr uint32_t MAX_CYCLE_MS = 40UL * 60UL * 1000UL;
  static constexpr uint32_t NEAR_WINDOW_BEFORE_MS = 2UL * 60UL * 1000UL;
  static constexpr uint32_t NEAR_WINDOW_AFTER_MS = 60UL * 1000UL;
  static constexpr uint32_t MISSED_WINDOW_MS = 2UL * 60UL * 1000UL;

  void reset();
  bool ingestPressureHpa(float pressureHpa, bool valid, uint32_t nowMs);
  void markArtifact(uint32_t nowMs, uint32_t durationMs);
  Snapshot snapshot(uint32_t nowMs) const;

  static const char* statusLabel(Status status);
  static const char* confidenceLabel(uint8_t confidencePercent);
  static const char* noiseLabel(float noiseHpa);

 private:
  struct Event {
    uint32_t timestampMs = 0;
    float intensity = 0.0F;
    uint8_t shapeScorePercent = 0;
    float residualHpa = 0.0F;
    float noiseHpa = 0.0F;
  };

  struct HistorySample {
    int16_t residualMilliHpa = 0;
    uint8_t flags = 0;
  };

  static constexpr uint8_t kMaxEvents = 12;
  static constexpr uint16_t kHistorySamples = 90U * 60U;
  static constexpr uint32_t kArtifactTouchMaskMs = 15UL * 1000UL;
  static constexpr uint32_t kMovementMaskMs = 60UL * 1000UL;
  static constexpr uint32_t kCorrelationUpdateMs = 30UL * 1000UL;
  static constexpr uint16_t kCorrelationMinLagSamples = MIN_CYCLE_MS / THERMAL_SAMPLE_PERIOD_MS;
  static constexpr uint16_t kCorrelationMaxLagSamples = MAX_CYCLE_MS / THERMAL_SAMPLE_PERIOD_MS;
  static constexpr uint16_t kCorrelationLagStepSamples = 30U;
  static constexpr uint16_t kCorrelationStrideSamples = 5U;
  static constexpr uint16_t kCorrelationWindowSamples = 60U * 60U;
  static constexpr uint16_t kCorrelationMinPairs = 72U;
  static constexpr uint8_t kHistoryFlagArtifact = 1U << 0;
  static constexpr uint8_t kHistoryFlagMovement = 1U << 1;
  static constexpr uint8_t kHistoryFlagNoisy = 1U << 2;
  static constexpr uint32_t kShortEmaTauMs = 30UL * 1000UL;
  static constexpr uint32_t kLongEmaTauMs = 12UL * 60UL * 1000UL;
  static constexpr uint32_t kNoiseTauMs = 3UL * 60UL * 1000UL;
  static constexpr uint32_t kDerivativeTauMs = 8UL * 1000UL;
  static constexpr float kEventSigmaFactor = 2.5F;
  static constexpr float kNoiseFloorHpa = 0.006F;
  static constexpr float kMinEventResidualHpa = 0.025F;
  static constexpr float kMinEventDPdtHpaPerSec = 0.0025F;
  static constexpr float kMaxNoisyPressureHpa = 0.40F;
  static constexpr float kMovementStepHpa = 0.18F;
  static constexpr float kPulseEnterFactor = 0.75F;
  static constexpr float kPulseReleaseFactor = 0.45F;
  static constexpr uint32_t kMinPulseDurationMs = 8UL * 1000UL;
  static constexpr uint32_t kMaxPulseDurationMs = 4UL * 60UL * 1000UL;
  static constexpr uint32_t kPossiblePulseVisibleMs = 25UL * 1000UL;
  static constexpr float kMinPulseAreaHpaSec = 0.16F;

  bool processOneSecondSample(float pressureHpa, uint32_t nowMs);
  void updatePulseDetector(uint32_t nowMs, uint32_t dtMs, float eventThreshold, float safeNoise);
  void resetPulse();
  void finishPulse(uint32_t nowMs, float eventThreshold, float safeNoise);
  void registerEvent(uint32_t eventMs, float intensity, uint8_t shapeScorePercent);
  void appendHistory(uint32_t nowMs, uint8_t flags);
  void updateCorrelation(uint32_t nowMs);
  bool historySampleFromLatest(uint16_t offset, HistorySample& sample) const;
  uint8_t collectIntervals(uint32_t* intervalsMs, uint8_t capacity) const;
  uint8_t chronologicalEvents(Event* events, uint8_t capacity) const;
  uint32_t medianInterval(uint32_t* intervalsMs, uint8_t count) const;
  uint32_t averageInterval(const uint32_t* intervalsMs, uint8_t count) const;
  float intervalVariation(const uint32_t* intervalsMs, uint8_t count, uint32_t medianMs) const;
  uint8_t computeQuality(uint32_t nowMs) const;
  uint8_t computeConfidence(uint32_t nowMs,
                            uint32_t medianMs,
                            float variation,
                            uint8_t intervalCount,
                            uint32_t bestPeriodMs,
                            uint8_t periodScorePercent,
                            uint8_t qualityPercent) const;
  float emaAlpha(uint32_t dtMs, uint32_t tauMs) const;
  bool warmupComplete(uint32_t nowMs) const;
  uint32_t elapsedSinceStart(uint32_t nowMs) const;

  bool initialized_ = false;
  bool pressureValid_ = false;
  uint32_t startedMs_ = 0;
  uint32_t lastSampleMs_ = 0;
  uint32_t windowStartMs_ = 0;
  uint32_t lastEventMs_ = 0;
  uint32_t artifactMaskUntilMs_ = 0;
  uint32_t movementLikelyUntilMs_ = 0;
  uint32_t lastCorrelationMs_ = 0;
  uint32_t lastPulseCandidateMs_ = 0;
  float sampleAccumulatorHpa_ = 0.0F;
  uint16_t sampleAccumulatorCount_ = 0;
  uint16_t processedSampleCount_ = 0;
  float currentPressureHpa_ = 0.0F;
  float previousPressureHpa_ = 0.0F;
  float shortEmaHpa_ = 0.0F;
  float longEmaHpa_ = 0.0F;
  float residualHpa_ = 0.0F;
  float previousResidualHpa_ = 0.0F;
  float derivativeEmaHpaPerSec_ = 0.0F;
  float noiseHpa_ = 0.0F;
  HistorySample history_[kHistorySamples] = {};
  uint16_t historyWriteIndex_ = 0;
  uint16_t historyCount_ = 0;
  uint32_t bestPeriodMs_ = 0;
  float bestCorrelation_ = 0.0F;
  uint8_t periodScorePercent_ = 0;
  bool pulseActive_ = false;
  uint32_t pulseStartMs_ = 0;
  uint32_t pulseLastMs_ = 0;
  uint32_t pulsePeakMs_ = 0;
  float pulsePeakAbsHpa_ = 0.0F;
  float pulsePeakResidualHpa_ = 0.0F;
  float pulsePeakNoiseHpa_ = 0.0F;
  float pulseAreaHpaSec_ = 0.0F;
  float pulseDerivativePeakHpaPerSec_ = 0.0F;
  Event events_[kMaxEvents] = {};
  uint8_t eventWriteIndex_ = 0;
  uint8_t eventCount_ = 0;
};
