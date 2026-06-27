#pragma once

#include <Arduino.h>
#include <stdint.h>

class VarioBuzzer {
 public:
  static constexpr uint8_t kMaxTonePoints = 12;
  static constexpr int8_t kMinResponseLevel = -5;
  static constexpr int8_t kMaxResponseLevel = 5;
  static constexpr int8_t kMinPitchLevel = -5;
  static constexpr int8_t kMaxPitchLevel = 5;
  static constexpr uint8_t kMinBeepVolumePercent = 0;
  static constexpr uint8_t kMaxBeepVolumePercent = 100;
  static constexpr uint8_t kBeepVolumeStepPercent = 10;

  struct TonePoint {
    TonePoint() = default;
    TonePoint(float vario, uint16_t frequency, uint16_t cycle, uint8_t duty)
        : varioMs(vario), frequencyHz(frequency), cycleMs(cycle), dutyPercent(duty) {}

    float varioMs = 0.0F;
    uint16_t frequencyHz = 0;
    uint16_t cycleMs = 0;
    uint8_t dutyPercent = 0;
  };

  struct AudioProfile {
    float climbToneOnThreshold = 0.10F;
    float climbToneOffThreshold = 0.05F;
    float sinkToneOnThreshold = -0.70F;
    float sinkToneOffThreshold = -0.60F;
    TonePoint tones[kMaxTonePoints] = {};
    uint8_t toneCount = 0;
    int8_t responseLevel = 0;
    int8_t pitchLevel = 0;
    uint8_t beepVolumePercent = kMaxBeepVolumePercent;
    bool voiceEnabled = true;
  };

  bool begin();
  void update(float varioMs, bool enabled);
  void stop();
  void playStartupSound();
  void playGpsConnectedSound();
  void playTakeoffSound();
  void playIdleWarningAlarm();
  void playTouchFeedback();
  void playPitchPreview();
  void playVolumePreview();
  AudioProfile profileSnapshot();
  bool adjustTonePoint(uint8_t index, int16_t frequencyDeltaHz, int16_t cycleDeltaMs, int8_t dutyDeltaPercent);
  bool adjustSimpleLevels(int8_t responseDelta, int8_t pitchDelta);
  bool adjustBeepVolume(int8_t deltaPercent);
  bool setBeepVolume(uint8_t volumePercent);
  bool toggleVoiceEnabled();
  bool saveProfile();
  void resetDefaultProfile();
  uint32_t taskStackFreeBytes() const;

 private:
  struct ToneSpec {
    uint16_t frequencyHz = 0;
    uint16_t cycleMs = 0;
    uint8_t dutyPercent = 0;
  };

  void setTone(uint16_t frequencyHz);
  void silence();
  void service(uint32_t now);
  void applyEnvelope(uint32_t now);
  void writeDuty(uint16_t duty);
  void playEventTone(uint16_t frequencyHz, uint16_t durationMs);
  void playEventToneWithDuty(uint16_t frequencyHz, uint16_t durationMs, uint16_t duty);
  void playGpsConnectedBeep();
  void configureVoicePwm();
  void restoreTonePwm();
  void writeVoiceDuty(uint8_t duty);
  void playVoiceSample(const uint8_t* samples, uint32_t sampleCount, uint16_t sampleRateHz);
  void playVoicePause(uint16_t durationMs);
  void resetToneState();
  void updateSmoothedVario(float varioMs, uint32_t now, const AudioProfile& profile);
  uint16_t sinkGlideFrequency(uint16_t targetFrequencyHz, uint32_t now);
  void resetSinkGlide();
  ToneSpec toneForVario(float varioMs, const AudioProfile& profile) const;
  bool shouldStartTone(float varioMs, const AudioProfile& profile) const;
  bool shouldStopTone(float varioMs, const AudioProfile& profile) const;
  void readTarget(float& varioMs, bool& enabled);
  void setDefaultProfileUnlocked();
  void applySimpleSettingsUnlocked(int8_t responseLevel, int8_t pitchLevel);
  bool loadProfile();
  bool isProfileValid(const AudioProfile& profile) const;
  static void audioTaskEntry(void* parameter);

  bool initialized_ = false;
  volatile bool eventSoundActive_ = false;
  volatile uint32_t eventSoundUntilMs_ = 0;
  bool toneLatched_ = false;
  bool pulseOn_ = false;
  bool targetEnabled_ = false;
  uint32_t phaseStartedMs_ = 0;
  uint32_t lastUpdateMs_ = 0;
  uint32_t lastEnvelopeMs_ = 0;
  float smoothedVarioMs_ = 0.0F;
  float targetVarioMs_ = 0.0F;
  AudioProfile profile_;
  ToneSpec activeTone_;
  uint16_t currentFrequencyHz_ = 0;
  float sinkGlideFrequencyHz_ = 0.0F;
  uint32_t lastSinkGlideMs_ = 0;
  uint16_t currentDuty_ = 0;
  uint16_t targetDuty_ = 0;
  TaskHandle_t audioTaskHandle_ = nullptr;
  SemaphoreHandle_t outputMutex_ = nullptr;
  portMUX_TYPE targetMux_ = portMUX_INITIALIZER_UNLOCKED;
  portMUX_TYPE profileMux_ = portMUX_INITIALIZER_UNLOCKED;
};
