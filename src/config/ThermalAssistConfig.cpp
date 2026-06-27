#include "config/ThermalAssistConfig.h"

#include <Preferences.h>

namespace {

static constexpr char kPrefsNamespace[] = "thermalCfg";
static constexpr char kKeyVersion[] = "ver";
static constexpr char kKeyVisualMode[] = "mode";
static constexpr char kKeyDriftMode[] = "drift";
static constexpr uint8_t kVersion = 2;

bool isValidMode(uint8_t value) {
  return value <= static_cast<uint8_t>(ThermalAssistVisualMode::ThermalCentered);
}

bool isValidDriftMode(uint8_t value) {
  return value <= static_cast<uint8_t>(ThermalAssistDriftMode::Particle);
}

}  // namespace

bool ThermalAssistConfig::begin() {
  if (!load()) {
    resetDefault();
    save();
    return false;
  }
  return true;
}

bool ThermalAssistConfig::load() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    return false;
  }

  const uint8_t version = prefs.getUChar(kKeyVersion, 0);
  const uint8_t mode = prefs.getUChar(kKeyVisualMode, 255);
  const uint8_t drift = prefs.getUChar(kKeyDriftMode, static_cast<uint8_t>(ThermalAssistDriftMode::Classic));
  prefs.end();

  if ((version != 1 && version != kVersion) || !isValidMode(mode) || !isValidDriftMode(drift)) {
    return false;
  }

  visualMode_ = static_cast<ThermalAssistVisualMode>(mode);
  driftMode_ = static_cast<ThermalAssistDriftMode>(drift);
  return true;
}

bool ThermalAssistConfig::save() const {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    Serial.println("Assistente termica: falha ao abrir NVS.");
    return false;
  }

  prefs.putUChar(kKeyVersion, kVersion);
  prefs.putUChar(kKeyVisualMode, static_cast<uint8_t>(visualMode_));
  prefs.putUChar(kKeyDriftMode, static_cast<uint8_t>(driftMode_));
  prefs.end();
  Serial.println("Assistente termica: modo visual salvo.");
  return true;
}

void ThermalAssistConfig::resetDefault() {
  visualMode_ = ThermalAssistVisualMode::PilotCentered;
  driftMode_ = ThermalAssistDriftMode::Classic;
}

const char* thermalAssistVisualModeLabel(ThermalAssistVisualMode mode) {
  switch (mode) {
    case ThermalAssistVisualMode::PilotCentered:
      return "PILOTO NO CENTRO";
    case ThermalAssistVisualMode::ThermalCentered:
      return "TERMICA NO CENTRO";
    default:
      return "MODO VISUAL";
  }
}

const char* thermalAssistDriftModeLabel(ThermalAssistDriftMode mode) {
  switch (mode) {
    case ThermalAssistDriftMode::Classic:
      return "DERIVA SIMPLES";
    case ThermalAssistDriftMode::Particle:
      return "DERIVA AVANCADA";
    default:
      return "DERIVA TERMICA";
  }
}
