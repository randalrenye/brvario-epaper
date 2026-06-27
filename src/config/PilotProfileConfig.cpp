#include "config/PilotProfileConfig.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

namespace {

static constexpr char kNamespace[] = "pilot";
static constexpr char kKeyVersion[] = "ver";
static constexpr char kKeyName[] = "name";
static constexpr char kKeyAge[] = "age";
static constexpr char kKeyEquipment[] = "equip";
static constexpr char kKeyGliderId[] = "gid";
static constexpr uint8_t kVersion = 1;

}  // namespace

bool PilotProfileConfig::begin() {
  if (!load()) {
    resetDefault();
    save();
    return false;
  }
  return true;
}

bool PilotProfileConfig::load() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) {
    return false;
  }

  const uint8_t version = prefs.getUChar(kKeyVersion, 0);
  if (version != kVersion) {
    prefs.end();
    return false;
  }

  prefs.getString(kKeyName, name_, sizeof(name_));
  prefs.getString(kKeyAge, age_, sizeof(age_));
  prefs.getString(kKeyEquipment, equipment_, sizeof(equipment_));
  prefs.getString(kKeyGliderId, gliderId_, sizeof(gliderId_));
  prefs.end();

  if (name_[0] == '\0') copySafe(name_, sizeof(name_), "PILOTO");
  if (equipment_[0] == '\0') copySafe(equipment_, sizeof(equipment_), "PARAPENTE");
  if (gliderId_[0] == '\0') copySafe(gliderId_, sizeof(gliderId_), "BRVARIO");
  return true;
}

bool PilotProfileConfig::save() const {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) {
    Serial.println("Piloto: falha ao abrir NVS.");
    return false;
  }
  prefs.putUChar(kKeyVersion, kVersion);
  prefs.putString(kKeyName, name_);
  prefs.putString(kKeyAge, age_);
  prefs.putString(kKeyEquipment, equipment_);
  prefs.putString(kKeyGliderId, gliderId_);
  prefs.end();
  return true;
}

void PilotProfileConfig::resetDefault() {
  copySafe(name_, sizeof(name_), "PILOTO");
  age_[0] = '\0';
  copySafe(equipment_, sizeof(equipment_), "PARAPENTE");
  copySafe(gliderId_, sizeof(gliderId_), "BRVARIO");
}

const char* PilotProfileConfig::fieldLabel(Field field) const {
  switch (field) {
    case Field::Name:
      return "NOME";
    case Field::Age:
      return "IDADE";
    case Field::Equipment:
      return "EQUIPAMENTO";
    case Field::GliderId:
      return "ID";
    default:
      return "";
  }
}

const char* PilotProfileConfig::fieldText(Field field) const {
  switch (field) {
    case Field::Name:
      return name_;
    case Field::Age:
      return age_;
    case Field::Equipment:
      return equipment_;
    case Field::GliderId:
      return gliderId_;
    default:
      return "";
  }
}

size_t PilotProfileConfig::fieldMaxLength(Field field) const {
  switch (field) {
    case Field::Name:
      return sizeof(name_) - 1;
    case Field::Age:
      return sizeof(age_) - 1;
    case Field::Equipment:
      return sizeof(equipment_) - 1;
    case Field::GliderId:
      return sizeof(gliderId_) - 1;
    default:
      return 0;
  }
}

bool PilotProfileConfig::appendChar(Field field, char value) {
  if (value < 32 || value > 126) {
    return false;
  }
  return appendTo(mutableField(field), fieldMaxLength(field) + 1, value);
}

bool PilotProfileConfig::backspace(Field field) {
  return backspaceText(mutableField(field));
}

bool PilotProfileConfig::clear(Field field) {
  char* text = mutableField(field);
  if (!text || text[0] == '\0') {
    return false;
  }
  text[0] = '\0';
  return true;
}

char* PilotProfileConfig::mutableField(Field field) {
  switch (field) {
    case Field::Name:
      return name_;
    case Field::Age:
      return age_;
    case Field::Equipment:
      return equipment_;
    case Field::GliderId:
      return gliderId_;
    default:
      return nullptr;
  }
}

bool PilotProfileConfig::appendTo(char* text, size_t capacity, char value) {
  if (!text || capacity == 0) {
    return false;
  }
  const size_t len = strlen(text);
  if (len + 1 >= capacity) {
    return false;
  }
  text[len] = value;
  text[len + 1] = '\0';
  return true;
}

bool PilotProfileConfig::backspaceText(char* text) {
  if (!text) {
    return false;
  }
  const size_t len = strlen(text);
  if (len == 0) {
    return false;
  }
  text[len - 1] = '\0';
  return true;
}

void PilotProfileConfig::copySafe(char* dst, size_t capacity, const char* src) {
  if (!dst || capacity == 0) {
    return;
  }
  size_t out = 0;
  if (src) {
    for (size_t i = 0; src[i] != '\0' && out + 1 < capacity; ++i) {
      const char c = src[i];
      if (c >= 32 && c <= 126) {
        dst[out++] = c;
      }
    }
  }
  dst[out] = '\0';
}
