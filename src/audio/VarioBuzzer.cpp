#include "audio/VarioBuzzer.h"

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <pgmspace.h>
#include <string.h>

#include "audio/GpsConnectedVoiceData.h"
#include "config/HardwareConfig.h"

#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

namespace {

class RecursiveSemaphoreGuard {
 public:
  RecursiveSemaphoreGuard(SemaphoreHandle_t semaphore, TickType_t timeoutTicks) : semaphore_(semaphore) {
    locked_ = semaphore_ != nullptr && xSemaphoreTakeRecursive(semaphore_, timeoutTicks) == pdTRUE;
  }

  ~RecursiveSemaphoreGuard() {
    if (locked_) {
      xSemaphoreGiveRecursive(semaphore_);
    }
  }

  RecursiveSemaphoreGuard(const RecursiveSemaphoreGuard&) = delete;
  RecursiveSemaphoreGuard& operator=(const RecursiveSemaphoreGuard&) = delete;

  bool locked() const { return locked_; }

 private:
  SemaphoreHandle_t semaphore_ = nullptr;
  bool locked_ = false;
};

static constexpr uint8_t kLedcChannel = 0;
static constexpr uint8_t kLedcResolutionBits = 10;
static constexpr uint16_t kBeepMinAudibleDuty = 8;
static constexpr uint16_t kBeepMaxUsefulDuty = 232;
static constexpr uint16_t kDutyAttackStep = 512;
static constexpr uint16_t kDutyReleaseStep = 256;
static constexpr uint32_t kAudioTaskIntervalMs = 2;
static constexpr uint8_t kVoiceResolutionBits = 8;
static constexpr uint32_t kVoicePwmCarrierHz = 62500UL;
static constexpr uint8_t kVoiceSilenceDuty = 128;
static constexpr uint8_t kVoiceNoiseGate = 4;
static constexpr uint16_t kVoiceBaseGainPercent = 240;
static constexpr uint16_t kVoiceFixedVolumePercent = 150;
static constexpr uint16_t kVoiceFadeMs = 18;
static constexpr uint16_t kTouchFeedbackFrequencyHz = 920;
static constexpr uint16_t kTouchFeedbackDurationMs = 26;
static constexpr uint16_t kTouchFeedbackMaxDuty = 34;
static constexpr uint32_t kSinkGlideUpdateMs = 18;
static constexpr float kSinkGlideRateHzPerSecond = 520.0F;

static constexpr float kAudioMinSlewRateMsPerSecond = 1.55F;
static constexpr float kAudioNormalSlewRateMsPerSecond = 1.85F;
static constexpr float kAudioMaxSlewRateMsPerSecond = 4.20F;
static constexpr char kAudioPrefsNamespace[] = "varioAudio";
static constexpr char kAudioPrefsKey[] = "profile";
static constexpr uint32_t kProfileMagic = 0x56415544UL;  // "VAUD"
static constexpr uint16_t kProfileVersion = 6;

struct StoredAudioProfile {
  uint32_t magic = kProfileMagic;
  uint16_t version = kProfileVersion;
  uint16_t reserved = 0;
  VarioBuzzer::AudioProfile profile = {};
  uint32_t checksum = 0;
};

const VarioBuzzer::TonePoint kSmoothToneTable[VarioBuzzer::kMaxTonePoints] = {
    {-10.0F, 200, 200, 100},
    {-3.0F, 293, 200, 100},
    {-2.0F, 369, 200, 100},
    {-0.94F, 374, 200, 100},
    {-0.50F, 370, 600, 100},
    {-0.01F, 339, 650, 49},
    {0.58F, 401, 595, 50},
    {1.29F, 486, 535, 50},
    {2.02F, 540, 435, 49},
    {2.83F, 575, 345, 50},
    {4.92F, 710, 270, 50},
    {10.00F, 911, 210, 50},
};

const VarioBuzzer::TonePoint kNormalToneTable[VarioBuzzer::kMaxTonePoints] = {
    {-10.0F, 200, 200, 100},
    {-3.0F, 293, 200, 100},
    {-2.0F, 369, 200, 100},
    {-1.0F, 440, 200, 100},
    {-0.5F, 475, 600, 100},
    {0.0F, 493, 600, 50},
    {0.5F, 550, 550, 50},
    {1.0F, 595, 500, 50},
    {2.0F, 675, 400, 50},
    {3.0F, 745, 310, 50},
    {5.0F, 880, 250, 50},
    {10.0F, 1108, 200, 50},
};

const VarioBuzzer::TonePoint kSensitiveToneTable[VarioBuzzer::kMaxTonePoints] = {
    {-10.0F, 200, 200, 100},
    {-3.0F, 293, 200, 100},
    {-2.0F, 369, 200, 100},
    {-1.02F, 412, 200, 100},
    {-0.50F, 420, 600, 100},
    {0.04F, 424, 600, 50},
    {0.50F, 470, 510, 50},
    {0.87F, 544, 437, 50},
    {1.53F, 656, 335, 50},
    {1.93F, 733, 313, 50},
    {3.06F, 1003, 155, 50},
    {10.00F, 1629, 83, 65},
};

float clampFloat(float value, float lo, float hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

uint16_t roundU16(float value) {
  if (value < 0.0F) return 0;
  return static_cast<uint16_t>(value + 0.5F);
}

uint8_t roundU8(float value) {
  if (value < 0.0F) return 0;
  if (value > 255.0F) return 255;
  return static_cast<uint8_t>(value + 0.5F);
}

uint16_t clampU16(int32_t value, uint16_t lo, uint16_t hi) {
  if (value < static_cast<int32_t>(lo)) return lo;
  if (value > static_cast<int32_t>(hi)) return hi;
  return static_cast<uint16_t>(value);
}

uint8_t clampU8(int32_t value, uint8_t lo, uint8_t hi) {
  if (value < static_cast<int32_t>(lo)) return lo;
  if (value > static_cast<int32_t>(hi)) return hi;
  return static_cast<uint8_t>(value);
}

int8_t clampI8(int32_t value, int8_t lo, int8_t hi) {
  if (value < static_cast<int32_t>(lo)) return lo;
  if (value > static_cast<int32_t>(hi)) return hi;
  return static_cast<int8_t>(value);
}

uint8_t amplifyVoiceSample(uint8_t sample, uint32_t sampleIndex, uint32_t sampleCount, uint32_t fadeSamples) {
  int32_t centered = static_cast<int32_t>(sample) - 128;

  const int32_t magnitude = abs(centered);
  if (magnitude <= kVoiceNoiseGate) {
    centered = 0;
  } else if (centered > 0) {
    centered -= kVoiceNoiseGate;
  } else {
    centered += kVoiceNoiseGate;
  }

  if (fadeSamples > 0) {
    uint32_t scale = 255;
    if (sampleIndex < fadeSamples) {
      scale = sampleIndex * 255UL / fadeSamples;
    }
    const uint32_t remaining = sampleCount - sampleIndex - 1;
    if (remaining < fadeSamples) {
      const uint32_t outScale = remaining * 255UL / fadeSamples;
      if (outScale < scale) scale = outScale;
    }
    centered = centered * static_cast<int32_t>(scale) / 255;
  }

  centered = centered * static_cast<int32_t>(kVoiceBaseGainPercent) * static_cast<int32_t>(kVoiceFixedVolumePercent) / 10000L;
  return clampU8(128 + centered, 0, 255);
}

uint16_t beepDutyForVolume(uint8_t volumePercent) {
  const uint8_t volume = clampU8(volumePercent, VarioBuzzer::kMinBeepVolumePercent, VarioBuzzer::kMaxBeepVolumePercent);
  if (volume == 0) {
    return 0;
  }

  // The S8050 + small speaker starts to compress before 50% duty.
  // Keeping the top end below that region makes 70-100% sound progressive.
  const uint32_t range = static_cast<uint32_t>(kBeepMaxUsefulDuty - kBeepMinAudibleDuty);
  const uint32_t duty = static_cast<uint32_t>(kBeepMinAudibleDuty) + (range * volume + 50UL) / 100UL;
  return static_cast<uint16_t>(duty);
}

uint32_t checksumProfile(const VarioBuzzer::AudioProfile& profile) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&profile);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < sizeof(profile); ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}

float pitchScale(int8_t level) {
  return clampFloat(1.0F + static_cast<float>(level) * 0.06F, 0.70F, 1.30F);
}

float audioSlewRate(const VarioBuzzer::AudioProfile& profile) {
  if (profile.responseLevel < 0) {
    const float t = static_cast<float>(profile.responseLevel - VarioBuzzer::kMinResponseLevel) /
                    static_cast<float>(-VarioBuzzer::kMinResponseLevel);
    return kAudioMinSlewRateMsPerSecond +
           (kAudioNormalSlewRateMsPerSecond - kAudioMinSlewRateMsPerSecond) * clampFloat(t, 0.0F, 1.0F);
  }

  const float t = static_cast<float>(profile.responseLevel) / static_cast<float>(VarioBuzzer::kMaxResponseLevel);
  return kAudioNormalSlewRateMsPerSecond +
         (kAudioMaxSlewRateMsPerSecond - kAudioNormalSlewRateMsPerSecond) * clampFloat(t, 0.0F, 1.0F);
}

VarioBuzzer::TonePoint blendTonePoint(const VarioBuzzer::TonePoint& a, const VarioBuzzer::TonePoint& b, float t) {
  t = clampFloat(t, 0.0F, 1.0F);
  VarioBuzzer::TonePoint result;
  result.varioMs = a.varioMs + (b.varioMs - a.varioMs) * t;
  result.frequencyHz = roundU16(static_cast<float>(a.frequencyHz) + (static_cast<float>(b.frequencyHz) - static_cast<float>(a.frequencyHz)) * t);
  result.cycleMs = roundU16(static_cast<float>(a.cycleMs) + (static_cast<float>(b.cycleMs) - static_cast<float>(a.cycleMs)) * t);
  result.dutyPercent = roundU8(static_cast<float>(a.dutyPercent) + (static_cast<float>(b.dutyPercent) - static_cast<float>(a.dutyPercent)) * t);
  return result;
}

}  // namespace

bool VarioBuzzer::begin() {
  if (!loadProfile()) {
    portENTER_CRITICAL(&profileMux_);
    setDefaultProfileUnlocked();
    portEXIT_CRITICAL(&profileMux_);
  }

  pinMode(HardwareConfig::kBuzzerPin, OUTPUT);
  digitalWrite(HardwareConfig::kBuzzerPin, LOW);
  Serial.printf("Buzzer passivo configurado no GPIO%d.\n", HardwareConfig::kBuzzerPin);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (!ledcAttach(HardwareConfig::kBuzzerPin, 1000, kLedcResolutionBits)) {
    initialized_ = false;
    Serial.printf("Buzzer: falha ao anexar LEDC ao GPIO%d.\n", HardwareConfig::kBuzzerPin);
    return false;
  }
#else
  ledcSetup(kLedcChannel, 1000, kLedcResolutionBits);
  ledcAttachPin(HardwareConfig::kBuzzerPin, kLedcChannel);
#endif

  initialized_ = true;
  silence();
  writeDuty(0);
  Serial.printf("Buzzer PWM iniciado no GPIO%d.\n", HardwareConfig::kBuzzerPin);

  outputMutex_ = xSemaphoreCreateRecursiveMutex();
  if (outputMutex_ == nullptr) {
    Serial.println("Buzzer: falha ao criar mutex de saida; task dedicada desativada.");
    return true;
  }

#if CONFIG_FREERTOS_UNICORE
  const BaseType_t taskCreated = xTaskCreate(audioTaskEntry, "vario_audio", 3072, this, 3, &audioTaskHandle_);
#else
  const BaseType_t taskCreated = xTaskCreatePinnedToCore(audioTaskEntry, "vario_audio", 3072, this, 3, &audioTaskHandle_, 0);
#endif
  if (taskCreated != pdPASS) {
    audioTaskHandle_ = nullptr;
    Serial.println("Buzzer: task dedicada nao iniciou; usando fallback no loop.");
  } else {
    Serial.println("Buzzer: task dedicada iniciada para cadencia estavel.");
  }

  return true;
}

void VarioBuzzer::playStartupSound() {
  if (!initialized_) return;
  RecursiveSemaphoreGuard output(outputMutex_, portMAX_DELAY);
  if (!output.locked()) return;

  eventSoundUntilMs_ = 0;
  eventSoundActive_ = true;
  for (uint16_t frequency = 200; frequency <= 1000; frequency += 100) {
    playEventTone(frequency, 60);
  }
  playEventTone(0, 0);
  eventSoundActive_ = false;
  resetToneState();
}

void VarioBuzzer::playGpsConnectedSound() {
  if (!initialized_) return;
  RecursiveSemaphoreGuard output(outputMutex_, portMAX_DELAY);
  if (!output.locked()) return;

  eventSoundUntilMs_ = 0;
  eventSoundActive_ = true;

  const AudioProfile profile = profileSnapshot();
  if (profile.voiceEnabled) {
    configureVoicePwm();
    playVoiceSample(GpsConnectedVoiceData::kPcm, GpsConnectedVoiceData::kSampleCount, GpsConnectedVoiceData::kSampleRateHz);
    playVoicePause(35);
    restoreTonePwm();
  } else {
    playGpsConnectedBeep();
  }

  eventSoundActive_ = false;
  resetToneState();
}

void VarioBuzzer::playTakeoffSound() {
  if (!initialized_) return;
  RecursiveSemaphoreGuard output(outputMutex_, portMAX_DELAY);
  if (!output.locked()) return;

  eventSoundUntilMs_ = 0;
  eventSoundActive_ = true;
  playEventTone(620, 120);
  delay(45);
  playEventTone(820, 120);
  delay(45);
  playEventTone(1040, 170);
  delay(55);
  playEventTone(1320, 220);
  playEventTone(0, 0);
  eventSoundActive_ = false;
  resetToneState();
}

void VarioBuzzer::playIdleWarningAlarm() {
  if (!initialized_) return;
  RecursiveSemaphoreGuard output(outputMutex_, pdMS_TO_TICKS(10));
  if (!output.locked() || eventSoundActive_) return;

  const AudioProfile profile = profileSnapshot();
  uint16_t duty = beepDutyForVolume(profile.beepVolumePercent);
  if (duty < 72) {
    duty = 72;
  }
  if (duty > kBeepMaxUsefulDuty) {
    duty = kBeepMaxUsefulDuty;
  }

  eventSoundUntilMs_ = 0;
  eventSoundActive_ = true;
  playEventToneWithDuty(760, 160, duty);
  delay(70);
  playEventToneWithDuty(1040, 160, duty);
  delay(70);
  playEventToneWithDuty(760, 240, duty);
  playEventToneWithDuty(0, 0, 0);
  eventSoundActive_ = false;
  resetToneState();
}

void VarioBuzzer::playTouchFeedback() {
  if (!initialized_) return;
  RecursiveSemaphoreGuard output(outputMutex_, pdMS_TO_TICKS(5));
  if (!output.locked() || eventSoundActive_) return;

  const AudioProfile profile = profileSnapshot();
  if (profile.beepVolumePercent == 0) return;

  uint16_t duty = beepDutyForVolume(profile.beepVolumePercent) / 7;
  if (duty > kTouchFeedbackMaxDuty) {
    duty = kTouchFeedbackMaxDuty;
  }
  if (duty > 0 && duty < kBeepMinAudibleDuty) {
    duty = kBeepMinAudibleDuty;
  }

  eventSoundUntilMs_ = millis() + kTouchFeedbackDurationMs;
  eventSoundActive_ = true;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(HardwareConfig::kBuzzerPin, kTouchFeedbackFrequencyHz);
  ledcWrite(HardwareConfig::kBuzzerPin, duty);
#else
  ledcWriteTone(kLedcChannel, kTouchFeedbackFrequencyHz);
  ledcWrite(kLedcChannel, duty);
#endif
  currentFrequencyHz_ = kTouchFeedbackFrequencyHz;
  currentDuty_ = duty;
  targetDuty_ = duty;
}

void VarioBuzzer::playPitchPreview() {
  if (!initialized_) return;
  RecursiveSemaphoreGuard output(outputMutex_, portMAX_DELAY);
  if (!output.locked()) return;

  const AudioProfile profile = profileSnapshot();
  const float previewVarioMs = profile.climbToneOnThreshold > 0.0F ? profile.climbToneOnThreshold : 0.10F;
  ToneSpec tone = toneForVario(previewVarioMs, profile);
  if (tone.frequencyHz == 0) {
    tone.frequencyHz = 600;
    tone.cycleMs = 420;
    tone.dutyPercent = 50;
  }

  uint16_t beepMs = static_cast<uint16_t>(static_cast<uint32_t>(tone.cycleMs) * tone.dutyPercent / 100UL);
  if (beepMs < 80) beepMs = 80;
  if (beepMs > 180) beepMs = 180;

  eventSoundUntilMs_ = 0;
  eventSoundActive_ = true;
  playEventTone(tone.frequencyHz, beepMs);
  delay(70);
  playEventTone(tone.frequencyHz, beepMs);
  playEventTone(0, 0);
  eventSoundActive_ = false;
  resetToneState();
}

void VarioBuzzer::playVolumePreview() {
  if (!initialized_) return;
  RecursiveSemaphoreGuard output(outputMutex_, portMAX_DELAY);
  if (!output.locked()) return;

  const AudioProfile profile = profileSnapshot();
  ToneSpec tone = toneForVario(0.50F, profile);
  if (tone.frequencyHz == 0) {
    tone.frequencyHz = 550;
  }

  eventSoundUntilMs_ = 0;
  eventSoundActive_ = true;
  playEventTone(tone.frequencyHz, 160);
  playEventTone(0, 0);
  eventSoundActive_ = false;
  resetToneState();
}

void VarioBuzzer::update(float varioMs, bool enabled) {
  if (!initialized_) return;

  const bool valid = enabled && isfinite(varioMs);
  portENTER_CRITICAL(&targetMux_);
  targetVarioMs_ = valid ? varioMs : 0.0F;
  targetEnabled_ = valid;
  portEXIT_CRITICAL(&targetMux_);

  if (audioTaskHandle_ == nullptr) {
    service(millis());
  }
}

VarioBuzzer::AudioProfile VarioBuzzer::profileSnapshot() {
  AudioProfile copy;
  portENTER_CRITICAL(&profileMux_);
  copy = profile_;
  portEXIT_CRITICAL(&profileMux_);
  return copy;
}

bool VarioBuzzer::adjustTonePoint(uint8_t index, int16_t frequencyDeltaHz, int16_t cycleDeltaMs, int8_t dutyDeltaPercent) {
  bool adjusted = false;
  portENTER_CRITICAL(&profileMux_);
  if (index < profile_.toneCount) {
    TonePoint& point = profile_.tones[index];
    point.frequencyHz = clampU16(static_cast<int32_t>(point.frequencyHz) + frequencyDeltaHz, 100, 4000);
    point.cycleMs = clampU16(static_cast<int32_t>(point.cycleMs) + cycleDeltaMs, 50, 2000);
    point.dutyPercent = clampU8(static_cast<int32_t>(point.dutyPercent) + dutyDeltaPercent, 1, 100);
    adjusted = true;
  }
  portEXIT_CRITICAL(&profileMux_);
  return adjusted;
}

bool VarioBuzzer::adjustSimpleLevels(int8_t responseDelta, int8_t pitchDelta) {
  RecursiveSemaphoreGuard output(outputMutex_, pdMS_TO_TICKS(50));
  if (!output.locked()) return false;

  AudioProfile current = profileSnapshot();
  const int8_t response = clampI8(static_cast<int32_t>(current.responseLevel) + responseDelta, kMinResponseLevel, kMaxResponseLevel);
  const int8_t pitch = clampI8(static_cast<int32_t>(current.pitchLevel) + pitchDelta, kMinPitchLevel, kMaxPitchLevel);
  const uint8_t beepVolume = clampU8(current.beepVolumePercent, kMinBeepVolumePercent, kMaxBeepVolumePercent);
  const bool voiceEnabled = current.voiceEnabled;

  portENTER_CRITICAL(&profileMux_);
  applySimpleSettingsUnlocked(response, pitch);
  profile_.beepVolumePercent = beepVolume;
  profile_.voiceEnabled = voiceEnabled;
  portEXIT_CRITICAL(&profileMux_);
  resetToneState();
  return true;
}

bool VarioBuzzer::adjustBeepVolume(int8_t deltaPercent) {
  uint8_t next = kMaxBeepVolumePercent;
  portENTER_CRITICAL(&profileMux_);
  next = clampU8(static_cast<int32_t>(profile_.beepVolumePercent) + deltaPercent, kMinBeepVolumePercent, kMaxBeepVolumePercent);
  portEXIT_CRITICAL(&profileMux_);
  return setBeepVolume(next);
}

bool VarioBuzzer::setBeepVolume(uint8_t volumePercent) {
  RecursiveSemaphoreGuard output(outputMutex_, pdMS_TO_TICKS(50));
  if (!output.locked()) return false;

  bool changed = false;
  portENTER_CRITICAL(&profileMux_);
  const uint8_t clamped = clampU8(volumePercent, kMinBeepVolumePercent, kMaxBeepVolumePercent);
  changed = clamped != profile_.beepVolumePercent;
  profile_.beepVolumePercent = clamped;
  portEXIT_CRITICAL(&profileMux_);
  resetToneState();
  return changed;
}

bool VarioBuzzer::toggleVoiceEnabled() {
  bool enabled = false;
  portENTER_CRITICAL(&profileMux_);
  profile_.voiceEnabled = !profile_.voiceEnabled;
  enabled = profile_.voiceEnabled;
  portEXIT_CRITICAL(&profileMux_);
  return enabled;
}

bool VarioBuzzer::saveProfile() {
  const AudioProfile profile = profileSnapshot();
  if (!isProfileValid(profile)) {
    Serial.println("Buzzer: perfil de audio invalido; nao foi salvo.");
    return false;
  }

  StoredAudioProfile stored;
  stored.profile = profile;
  stored.checksum = checksumProfile(stored.profile);

  Preferences prefs;
  if (!prefs.begin(kAudioPrefsNamespace, false)) {
    Serial.println("Buzzer: falha ao abrir NVS para salvar audio.");
    return false;
  }
  const size_t written = prefs.putBytes(kAudioPrefsKey, &stored, sizeof(stored));
  prefs.end();

  const bool ok = written == sizeof(stored);
  Serial.println(ok ? "Buzzer: perfil de audio salvo." : "Buzzer: falha ao salvar perfil de audio.");
  return ok;
}

void VarioBuzzer::resetDefaultProfile() {
  RecursiveSemaphoreGuard output(outputMutex_, pdMS_TO_TICKS(50));
  if (!output.locked()) return;

  portENTER_CRITICAL(&profileMux_);
  setDefaultProfileUnlocked();
  portEXIT_CRITICAL(&profileMux_);
  resetToneState();
}

void VarioBuzzer::service(uint32_t now) {
  RecursiveSemaphoreGuard output(outputMutex_, 0);
  if (!output.locked()) {
    return;
  }

  if (eventSoundActive_) {
    if (eventSoundUntilMs_ == 0 || static_cast<int32_t>(now - eventSoundUntilMs_) < 0) {
      return;
    }
    eventSoundUntilMs_ = 0;
    eventSoundActive_ = false;
    resetToneState();
  }

  float varioMs = 0.0F;
  bool enabled = false;
  readTarget(varioMs, enabled);
  const AudioProfile profile = profileSnapshot();

  if (!enabled || !isfinite(varioMs)) {
    resetToneState();
    smoothedVarioMs_ = 0.0F;
    lastUpdateMs_ = 0;
    applyEnvelope(now);
    return;
  }

  updateSmoothedVario(varioMs, now, profile);

  if (!toneLatched_) {
    if (!shouldStartTone(smoothedVarioMs_, profile)) {
      silence();
      applyEnvelope(now);
      return;
    }
    toneLatched_ = true;
    pulseOn_ = true;
    activeTone_ = toneForVario(smoothedVarioMs_, profile);
    phaseStartedMs_ = now;
  } else if (shouldStopTone(smoothedVarioMs_, profile)) {
    resetToneState();
    applyEnvelope(now);
    return;
  }

  const ToneSpec tone = toneForVario(smoothedVarioMs_, profile);
  if (tone.frequencyHz == 0 || tone.dutyPercent == 0) {
    resetToneState();
    applyEnvelope(now);
    return;
  }

  const bool sinkToneActive = smoothedVarioMs_ <= profile.sinkToneOffThreshold;
  if (!sinkToneActive) {
    resetSinkGlide();
  }

  if (tone.dutyPercent >= 99 || tone.cycleMs == 0) {
    pulseOn_ = true;
    activeTone_ = tone;
    const uint16_t frequencyHz =
        sinkToneActive ? sinkGlideFrequency(activeTone_.frequencyHz, now) : activeTone_.frequencyHz;
    setTone(frequencyHz);
    applyEnvelope(now);
    return;
  }

  if (activeTone_.frequencyHz == 0 || activeTone_.dutyPercent == 0) {
    activeTone_ = tone;
  }

  const uint32_t onMs = static_cast<uint32_t>(activeTone_.cycleMs) * activeTone_.dutyPercent / 100UL;
  const uint32_t offMs = activeTone_.cycleMs > onMs ? activeTone_.cycleMs - onMs : 0UL;

  if (pulseOn_) {
    const uint16_t frequencyHz =
        sinkToneActive ? sinkGlideFrequency(activeTone_.frequencyHz, now) : activeTone_.frequencyHz;
    setTone(frequencyHz);
    if (now - phaseStartedMs_ >= onMs) {
      pulseOn_ = false;
      phaseStartedMs_ = now;
      silence();
    }
  } else {
    silence();
    if (now - phaseStartedMs_ >= offMs) {
      pulseOn_ = true;
      activeTone_ = tone;
      phaseStartedMs_ = now;
      const uint16_t frequencyHz = sinkToneActive ? sinkGlideFrequency(activeTone_.frequencyHz, now)
                                                  : activeTone_.frequencyHz;
      setTone(frequencyHz);
    }
  }

  applyEnvelope(now);
}

void VarioBuzzer::stop() {
  portENTER_CRITICAL(&targetMux_);
  targetEnabled_ = false;
  targetVarioMs_ = 0.0F;
  portEXIT_CRITICAL(&targetMux_);

  if (audioTaskHandle_ == nullptr) {
    RecursiveSemaphoreGuard output(outputMutex_, pdMS_TO_TICKS(20));
    if (!output.locked()) return;
    resetToneState();
  }
}

uint32_t VarioBuzzer::taskStackFreeBytes() const {
  TaskHandle_t handle = audioTaskHandle_;
  if (handle == nullptr) {
    return 0;
  }
  return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(handle));
}

void VarioBuzzer::resetToneState() {
  silence();
  toneLatched_ = false;
  pulseOn_ = false;
  activeTone_ = {};
  resetSinkGlide();
  phaseStartedMs_ = millis();
}

void VarioBuzzer::setTone(uint16_t frequencyHz) {
  if (frequencyHz != currentFrequencyHz_) {
    currentFrequencyHz_ = frequencyHz;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWriteTone(HardwareConfig::kBuzzerPin, frequencyHz);
#else
    ledcWriteTone(kLedcChannel, frequencyHz);
#endif
  }

  const AudioProfile profile = profileSnapshot();
  targetDuty_ = beepDutyForVolume(profile.beepVolumePercent);
}

void VarioBuzzer::silence() {
  targetDuty_ = 0;
}

void VarioBuzzer::applyEnvelope(uint32_t now) {
  if (lastEnvelopeMs_ != 0 && now - lastEnvelopeMs_ < kAudioTaskIntervalMs) return;
  lastEnvelopeMs_ = now;

  uint16_t nextDuty = currentDuty_;
  if (currentDuty_ < targetDuty_) {
    const uint16_t delta = targetDuty_ - currentDuty_;
    nextDuty = currentDuty_ + (delta > kDutyAttackStep ? kDutyAttackStep : delta);
  } else if (currentDuty_ > targetDuty_) {
    const uint16_t delta = currentDuty_ - targetDuty_;
    nextDuty = currentDuty_ - (delta > kDutyReleaseStep ? kDutyReleaseStep : delta);
  }

  if (nextDuty != currentDuty_) {
    writeDuty(nextDuty);
  }
}

void VarioBuzzer::writeDuty(uint16_t duty) {
  currentDuty_ = duty;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(HardwareConfig::kBuzzerPin, duty);
#else
  ledcWrite(kLedcChannel, duty);
#endif
}

void VarioBuzzer::playEventTone(uint16_t frequencyHz, uint16_t durationMs) {
  const AudioProfile profile = profileSnapshot();
  const uint16_t duty = frequencyHz == 0 ? 0 : beepDutyForVolume(profile.beepVolumePercent);
  playEventToneWithDuty(frequencyHz, durationMs, duty);
}

void VarioBuzzer::playEventToneWithDuty(uint16_t frequencyHz, uint16_t durationMs, uint16_t duty) {
  if (frequencyHz == 0) {
    duty = 0;
  }
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(HardwareConfig::kBuzzerPin, frequencyHz);
  ledcWrite(HardwareConfig::kBuzzerPin, duty);
#else
  ledcWriteTone(kLedcChannel, frequencyHz);
  ledcWrite(kLedcChannel, duty);
#endif
  currentFrequencyHz_ = frequencyHz;
  currentDuty_ = duty;
  targetDuty_ = currentDuty_;
  if (durationMs > 0) {
    delay(durationMs);
  }
}

void VarioBuzzer::playGpsConnectedBeep() {
  playEventTone(520, 90);
  delay(45);
  playEventTone(780, 90);
  delay(45);
  playEventTone(1040, 150);
  playEventTone(0, 0);
}

void VarioBuzzer::configureVoicePwm() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(HardwareConfig::kBuzzerPin, kVoicePwmCarrierHz, kVoiceResolutionBits);
#else
  ledcSetup(kLedcChannel, kVoicePwmCarrierHz, kVoiceResolutionBits);
  ledcAttachPin(HardwareConfig::kBuzzerPin, kLedcChannel);
#endif
  currentFrequencyHz_ = 0;
  currentDuty_ = 0;
  targetDuty_ = 0;
  writeVoiceDuty(kVoiceSilenceDuty);
  delay(6);
}

void VarioBuzzer::restoreTonePwm() {
  writeVoiceDuty(kVoiceSilenceDuty);
  delay(4);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(HardwareConfig::kBuzzerPin, 1000, kLedcResolutionBits);
#else
  ledcSetup(kLedcChannel, 1000, kLedcResolutionBits);
  ledcAttachPin(HardwareConfig::kBuzzerPin, kLedcChannel);
#endif
  currentFrequencyHz_ = 0;
  currentDuty_ = 0;
  targetDuty_ = 0;
  silence();
  writeDuty(0);
}

void VarioBuzzer::writeVoiceDuty(uint8_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(HardwareConfig::kBuzzerPin, duty);
#else
  ledcWrite(kLedcChannel, duty);
#endif
}

void VarioBuzzer::playVoiceSample(const uint8_t* samples, uint32_t sampleCount, uint16_t sampleRateHz) {
  if (samples == nullptr || sampleCount == 0 || sampleRateHz == 0) {
    return;
  }

  const uint32_t samplePeriodUs = 1000000UL / static_cast<uint32_t>(sampleRateHz);
  const uint32_t fadeSamples = (static_cast<uint32_t>(sampleRateHz) * kVoiceFadeMs) / 1000UL;
  uint32_t nextSampleUs = micros();
  for (uint32_t i = 0; i < sampleCount; ++i) {
    const uint8_t sample = pgm_read_byte(samples + i);
    writeVoiceDuty(amplifyVoiceSample(sample, i, sampleCount, fadeSamples));
    nextSampleUs += samplePeriodUs;
    while (static_cast<int32_t>(micros() - nextSampleUs) < 0) {
      // Busy wait keeps voice timing steadier than delay() on short PCM samples.
    }
    if ((i & 0x07FF) == 0) {
      delay(0);
    }
  }
}

void VarioBuzzer::playVoicePause(uint16_t durationMs) {
  writeVoiceDuty(kVoiceSilenceDuty);
  if (durationMs > 0) {
    delay(durationMs);
  }
}

void VarioBuzzer::updateSmoothedVario(float varioMs, uint32_t now, const AudioProfile& profile) {
  if (lastUpdateMs_ == 0) {
    smoothedVarioMs_ = varioMs;
    lastUpdateMs_ = now;
    return;
  }

  const float dt = static_cast<float>(now - lastUpdateMs_) / 1000.0F;
  lastUpdateMs_ = now;
  const float maxStep = audioSlewRate(profile) * clampFloat(dt, 0.0F, 0.1F);
  const float delta = varioMs - smoothedVarioMs_;
  if (delta > maxStep) {
    smoothedVarioMs_ += maxStep;
  } else if (delta < -maxStep) {
    smoothedVarioMs_ -= maxStep;
  } else {
    smoothedVarioMs_ = varioMs;
  }
}

uint16_t VarioBuzzer::sinkGlideFrequency(uint16_t targetFrequencyHz, uint32_t now) {
  if (targetFrequencyHz == 0) {
    resetSinkGlide();
    return 0;
  }

  if (sinkGlideFrequencyHz_ <= 0.0F || lastSinkGlideMs_ == 0) {
    sinkGlideFrequencyHz_ = currentFrequencyHz_ > 0 ? static_cast<float>(currentFrequencyHz_)
                                                     : static_cast<float>(targetFrequencyHz);
    lastSinkGlideMs_ = now;
  }

  if (now - lastSinkGlideMs_ < kSinkGlideUpdateMs) {
    return roundU16(sinkGlideFrequencyHz_);
  }

  const float dt = clampFloat(static_cast<float>(now - lastSinkGlideMs_) / 1000.0F, 0.0F, 0.12F);
  lastSinkGlideMs_ = now;

  const float target = static_cast<float>(targetFrequencyHz);
  const float delta = target - sinkGlideFrequencyHz_;
  const float maxStep = kSinkGlideRateHzPerSecond * dt;

  if (fabsf(delta) <= maxStep) {
    sinkGlideFrequencyHz_ = target;
  } else if (delta > 0.0F) {
    sinkGlideFrequencyHz_ += maxStep;
  } else {
    sinkGlideFrequencyHz_ -= maxStep;
  }

  return roundU16(sinkGlideFrequencyHz_);
}

void VarioBuzzer::resetSinkGlide() {
  sinkGlideFrequencyHz_ = 0.0F;
  lastSinkGlideMs_ = 0;
}

VarioBuzzer::ToneSpec VarioBuzzer::toneForVario(float varioMs, const AudioProfile& profile) const {
  const size_t count = profile.toneCount;
  if (count == 0) return {};

  if (varioMs <= profile.tones[0].varioMs) {
    ToneSpec tone;
    tone.frequencyHz = profile.tones[0].frequencyHz;
    tone.cycleMs = profile.tones[0].cycleMs;
    tone.dutyPercent = profile.tones[0].dutyPercent;
    return tone;
  }
  if (varioMs >= profile.tones[count - 1].varioMs) {
    ToneSpec tone;
    tone.frequencyHz = profile.tones[count - 1].frequencyHz;
    tone.cycleMs = profile.tones[count - 1].cycleMs;
    tone.dutyPercent = profile.tones[count - 1].dutyPercent;
    return tone;
  }

  for (size_t i = 0; i < count - 1; ++i) {
    const TonePoint& a = profile.tones[i];
    const TonePoint& b = profile.tones[i + 1];
    if (varioMs > b.varioMs) continue;

    const float scale = (varioMs - a.varioMs) / (b.varioMs - a.varioMs);
    ToneSpec tone;
    tone.frequencyHz = roundU16(static_cast<float>(a.frequencyHz) + scale * (static_cast<float>(b.frequencyHz) - a.frequencyHz));
    tone.cycleMs = roundU16(static_cast<float>(a.cycleMs) + scale * (static_cast<float>(b.cycleMs) - a.cycleMs));
    tone.dutyPercent = roundU8(static_cast<float>(a.dutyPercent) + scale * (static_cast<float>(b.dutyPercent) - a.dutyPercent));
    return tone;
  }

  return {};
}

bool VarioBuzzer::shouldStartTone(float varioMs, const AudioProfile& profile) const {
  return varioMs >= profile.climbToneOnThreshold || varioMs <= profile.sinkToneOnThreshold;
}

bool VarioBuzzer::shouldStopTone(float varioMs, const AudioProfile& profile) const {
  return varioMs < profile.climbToneOffThreshold && varioMs > profile.sinkToneOffThreshold;
}

void VarioBuzzer::readTarget(float& varioMs, bool& enabled) {
  portENTER_CRITICAL(&targetMux_);
  varioMs = targetVarioMs_;
  enabled = targetEnabled_;
  portEXIT_CRITICAL(&targetMux_);
}

void VarioBuzzer::setDefaultProfileUnlocked() {
  applySimpleSettingsUnlocked(0, 0);
}

void VarioBuzzer::applySimpleSettingsUnlocked(int8_t responseLevel, int8_t pitchLevel) {
  responseLevel = clampI8(responseLevel, kMinResponseLevel, kMaxResponseLevel);
  pitchLevel = clampI8(pitchLevel, kMinPitchLevel, kMaxPitchLevel);

  profile_ = {};
  profile_.climbToneOnThreshold = 0.10F;
  profile_.climbToneOffThreshold = 0.05F;
  profile_.sinkToneOnThreshold = -0.70F;
  profile_.sinkToneOffThreshold = -0.60F;
  profile_.beepVolumePercent = kMaxBeepVolumePercent;
  profile_.voiceEnabled = true;

  const float freqScale = pitchScale(pitchLevel);
  profile_.toneCount = kMaxTonePoints;
  profile_.responseLevel = responseLevel;
  profile_.pitchLevel = pitchLevel;

  for (uint8_t i = 0; i < kMaxTonePoints; ++i) {
    TonePoint adjusted;
    if (responseLevel < 0) {
      const float t = static_cast<float>(responseLevel - kMinResponseLevel) / static_cast<float>(-kMinResponseLevel);
      adjusted = blendTonePoint(kSmoothToneTable[i], kNormalToneTable[i], t);
    } else {
      const float t = static_cast<float>(responseLevel) / static_cast<float>(kMaxResponseLevel);
      adjusted = blendTonePoint(kNormalToneTable[i], kSensitiveToneTable[i], t);
    }
    adjusted.frequencyHz = clampU16(roundU16(static_cast<float>(adjusted.frequencyHz) * freqScale), 100, 4000);
    adjusted.cycleMs = clampU16(adjusted.cycleMs, 50, 2000);
    adjusted.dutyPercent = clampU8(adjusted.dutyPercent, 1, 100);
    profile_.tones[i] = adjusted;
  }
}

bool VarioBuzzer::loadProfile() {
  Preferences prefs;
  if (!prefs.begin(kAudioPrefsNamespace, true)) {
    return false;
  }

  const size_t length = prefs.getBytesLength(kAudioPrefsKey);
  if (length != sizeof(StoredAudioProfile)) {
    prefs.end();
    return false;
  }

  StoredAudioProfile stored;
  const size_t read = prefs.getBytes(kAudioPrefsKey, &stored, sizeof(stored));
  prefs.end();

  if (read != sizeof(stored) || stored.magic != kProfileMagic || stored.version != kProfileVersion) {
    return false;
  }
  if (stored.checksum != checksumProfile(stored.profile) || !isProfileValid(stored.profile)) {
    Serial.println("Buzzer: perfil salvo invalido; usando padrao.");
    return false;
  }

  const int8_t responseLevel = stored.profile.responseLevel;
  const int8_t pitchLevel = stored.profile.pitchLevel;
  const uint8_t beepVolume = stored.profile.beepVolumePercent;
  const bool voiceEnabled = stored.profile.voiceEnabled;

  portENTER_CRITICAL(&profileMux_);
  applySimpleSettingsUnlocked(responseLevel, pitchLevel);
  profile_.beepVolumePercent = beepVolume;
  profile_.voiceEnabled = voiceEnabled;
  portEXIT_CRITICAL(&profileMux_);
  Serial.println("Buzzer: perfil de audio carregado da memoria e recalculado.");
  return true;
}

bool VarioBuzzer::isProfileValid(const AudioProfile& profile) const {
  if (!isfinite(profile.climbToneOnThreshold) || !isfinite(profile.climbToneOffThreshold) ||
      !isfinite(profile.sinkToneOnThreshold) || !isfinite(profile.sinkToneOffThreshold)) {
    return false;
  }
  if (profile.climbToneOffThreshold > profile.climbToneOnThreshold) return false;
  if (profile.sinkToneOnThreshold > profile.sinkToneOffThreshold) return false;
  if (profile.toneCount < 2 || profile.toneCount > kMaxTonePoints) return false;
  if (profile.responseLevel < kMinResponseLevel || profile.responseLevel > kMaxResponseLevel) return false;
  if (profile.pitchLevel < kMinPitchLevel || profile.pitchLevel > kMaxPitchLevel) return false;
  if (profile.beepVolumePercent < kMinBeepVolumePercent || profile.beepVolumePercent > kMaxBeepVolumePercent) return false;

  float previousVario = profile.tones[0].varioMs;
  for (uint8_t i = 0; i < profile.toneCount; ++i) {
    const TonePoint& point = profile.tones[i];
    if (!isfinite(point.varioMs)) return false;
    if (i > 0 && point.varioMs <= previousVario) return false;
    previousVario = point.varioMs;
    if (point.frequencyHz < 100 || point.frequencyHz > 4000) return false;
    if (point.cycleMs < 50 || point.cycleMs > 2000) return false;
    if (point.dutyPercent < 1 || point.dutyPercent > 100) return false;
  }
  return true;
}

void VarioBuzzer::audioTaskEntry(void* parameter) {
  VarioBuzzer* buzzer = static_cast<VarioBuzzer*>(parameter);
  TickType_t lastWake = xTaskGetTickCount();

  while (true) {
    if (buzzer) {
      buzzer->service(millis());
    }
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(kAudioTaskIntervalMs));
  }
}
