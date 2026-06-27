#pragma once

#include <Arduino.h>

#include "data/VarioData.h"
#include "filters/VarioKalman.h"

class Bmp280Barometer {
 public:
  using RealtimeSampleCallback = void (*)(float varioMs, bool valid, void* context);

  bool begin();
  void end();
  bool poll();
  bool startRealtimePolling(RealtimeSampleCallback callback, void* context);
  bool realtimePollingActive() const { return pollingTaskHandle_ != nullptr; }
  void applyTo(VarioData& data) const;
  void printStatus(Stream& out) const;

  bool isReady() const { return ready_; }
  bool hasSample() const;
  bool audioDataValid() const;
  float audioVarioMs() const;
  uint32_t taskStackFreeBytes() const;

 private:
  static constexpr uint8_t kAddressLow = 0x76;
  static constexpr uint8_t kAddressHigh = 0x77;
  static constexpr uint8_t kBmp280ChipId = 0x58;
  static constexpr uint8_t kBme280ChipId = 0x60;
  static constexpr uint32_t kSampleIntervalMs = 50;
  static constexpr uint32_t kStaleSampleMs = 2000;
  static constexpr uint32_t kAudioStaleSampleMs = 250;
  static constexpr uint32_t kAudioRepublishMs = 80;
  static constexpr float kDefaultQnhHpa = 1013.25F;

  uint8_t address_ = 0;
  uint8_t chipId_ = 0;
  BarometerSensorStatus status_ = BarometerSensorStatus::Off;
  bool ready_ = false;
  bool sampleValid_ = false;
  bool groundReferenceSet_ = false;
  uint32_t sampleCount_ = 0;
  uint32_t failedReadCount_ = 0;
  uint32_t lastSampleMs_ = 0;
  uint32_t lastHistoryMs_ = 0;
  float pressureHpa_ = kDefaultQnhHpa;
  float temperatureC_ = 0.0F;
  float rawAltitudeM_ = 0.0F;
  float audioVarioMs_ = 0.0F;
  float groundAltitudeReferenceM_ = 0.0F;
  float history_[kVarioHistorySamples] = {};
  uint8_t historyCount_ = 0;
  VarioKalman filter_;
  mutable portMUX_TYPE dataMux_ = portMUX_INITIALIZER_UNLOCKED;
  TaskHandle_t volatile pollingTaskHandle_ = nullptr;
  volatile bool pollingStopRequested_ = false;
  RealtimeSampleCallback realtimeSampleCallback_ = nullptr;
  void* realtimeSampleContext_ = nullptr;

  uint16_t digT1_ = 0;
  int16_t digT2_ = 0;
  int16_t digT3_ = 0;
  uint16_t digP1_ = 0;
  int16_t digP2_ = 0;
  int16_t digP3_ = 0;
  int16_t digP4_ = 0;
  int16_t digP5_ = 0;
  int16_t digP6_ = 0;
  int16_t digP7_ = 0;
  int16_t digP8_ = 0;
  int16_t digP9_ = 0;
  int32_t tFine_ = 0;

  bool detect();
  void printI2cScan(Stream& out);
  bool configure();
  bool readCalibration();
  bool readSample(uint32_t now);
  bool readRaw(int32_t& adcT, int32_t& adcP);
  int32_t compensateTemperature(int32_t adcT);
  uint32_t compensatePressure(int32_t adcP);
  float pressureToAltitude(float pressureHpa) const;
  void pushHistory(float varioMs, uint32_t now);

  bool writeRegister(uint8_t reg, uint8_t value);
  bool readRegister(uint8_t reg, uint8_t& value);
  bool readBytes(uint8_t reg, uint8_t* buffer, uint8_t length);
  void stopRealtimePolling();
  static void pollingTaskEntry(void* parameter);
};
