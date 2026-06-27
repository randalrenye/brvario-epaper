#pragma once

#include <Arduino.h>
#include <stdint.h>

enum class ThermalAssistVisualMode : uint8_t {
  PilotCentered = 0,
  ThermalCentered = 1,
};

enum class ThermalAssistDriftMode : uint8_t {
  Classic = 0,
  Particle = 1,
};

class ThermalAssistConfig {
 public:
  bool begin();
  bool save() const;
  void resetDefault();

  ThermalAssistVisualMode visualMode() const { return visualMode_; }
  void setVisualMode(ThermalAssistVisualMode mode) { visualMode_ = mode; }
  ThermalAssistDriftMode driftMode() const { return driftMode_; }
  void setDriftMode(ThermalAssistDriftMode mode) { driftMode_ = mode; }

 private:
  ThermalAssistVisualMode visualMode_ = ThermalAssistVisualMode::PilotCentered;
  ThermalAssistDriftMode driftMode_ = ThermalAssistDriftMode::Classic;

  bool load();
};

const char* thermalAssistVisualModeLabel(ThermalAssistVisualMode mode);
const char* thermalAssistDriftModeLabel(ThermalAssistDriftMode mode);
