#pragma once

#include <stdint.h>

#include "config/ThermalAssistConfig.h"
#include "navigation/WindEstimator.h"

static constexpr uint8_t kVarioHistorySamples = 56;
static constexpr uint8_t kThermalAssistPoints = 24;
static constexpr uint8_t kThermalHistoryPoints = 8;

enum class GpsSensorStatus : uint8_t {
  Off,
  NoData,
  StaleData,
  NoFix,
  Fix,
};

enum class BarometerSensorStatus : uint8_t {
  Off,
  NotFound,
  CalibrationError,
  ConfigError,
  NoSample,
  StaleSample,
  ReadError,
  Ok,
};

struct ThermalAssistPoint {
  float eastM;
  float northM;
  float liftMs;
};

struct ThermalHistoryPoint {
  float eastM = 0.0F;
  float northM = 0.0F;
  float coreMs = 0.0F;
  uint8_t confidencePercent = 0;
  uint8_t ageMinutes = 0;
  bool active = false;
};

struct VarioData {
  VarioData() = default;

  float varioMs = 0.0F;
  float ganhoTermicaM = 0.0F;
  float thermalCoreMs = 0.0F;
  float thermalDriftDeg = 0.0F;
  float thermalRangeM = 120.0F;
  float thermalPilotEastM = 0.0F;
  float thermalPilotNorthM = 0.0F;
  float altitudeM = 0.0F;
  float altitudeGpsM = 0.0F;
  float altitudeGpsAglM = 0.0F;
  float altitudeBaroAglM = 0.0F;
  float altitudeAglM = 0.0F;
  float latitudeDeg = 0.0F;
  float longitudeDeg = 0.0F;
  float groundSpeedKmh = 0.0F;
  float airSpeedKmh = 0.0F;
  float windSpeedKmh = 0.0F;
  float windDirectionDeg = 0.0F;
  float windDirectionToDeg = 180.0F;
  float glideRatio = 0.0F;
  float courseDeg = 0.0F;
  float pressureHpa = 1013.25F;
  float qnhHpa = 1013.25F;
  float temperatureC = 20.0F;
  float batteryVoltage = 4.05F;
  float gpsHdop = 99.9F;
  uint8_t batteryPercent = 100;
  uint8_t thermalLockPercent = 0;
  uint8_t thermalCoreConfidencePercent = 0;
  uint8_t satellites = 0;
  uint32_t timeOfDaySeconds = 0;
  uint32_t gpsLastSentenceAgeMs = 0;
  uint32_t gpsLastFixAgeMs = 0;
  uint32_t barometerSampleAgeMs = 0;
  uint32_t barometerFailedReads = 0;
  uint32_t barometerSampleCount = 0;
  uint8_t barometerI2cAddress = 0;
  uint8_t barometerChipId = 0;
  bool gpsFix = false;
  bool wifiEnabled = false;
  bool bluetoothActive = false;
  bool bluetoothConnected = false;
  bool batteryCharging = false;
  bool audioEnabled = true;
  bool trackingEnabled = false;
  bool sensorDataValid = false;
  GpsSensorStatus gpsStatus = GpsSensorStatus::Off;
  BarometerSensorStatus barometerStatus = BarometerSensorStatus::Off;
  WindQuality windQuality = WindQuality::None;
  ThermalAssistVisualMode thermalVisualMode = ThermalAssistVisualMode::PilotCentered;
  ThermalAssistDriftMode thermalDriftMode = ThermalAssistDriftMode::Classic;
  uint32_t elapsedSeconds = 0;
  float varioHistory[kVarioHistorySamples] = {};
  uint8_t historyCount = 0;
  ThermalAssistPoint thermalPoints[kThermalAssistPoints] = {};
  uint8_t thermalPointCount = 0;
  ThermalHistoryPoint thermalHistory[kThermalHistoryPoints] = {};
  uint8_t thermalHistoryCount = 0;
};
