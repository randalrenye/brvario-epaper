#pragma once

#include <stdint.h>
#include <stddef.h>

class PilotProfileConfig {
 public:
  enum class Field : uint8_t {
    Name = 0,
    Age,
    Equipment,
    GliderId,
    Count,
  };

  static constexpr size_t kNameSize = 32;
  static constexpr size_t kAgeSize = 4;
  static constexpr size_t kEquipmentSize = 32;
  static constexpr size_t kGliderIdSize = 16;

  bool begin();
  bool save() const;
  void resetDefault();

  const char* pilotName() const { return name_; }
  const char* age() const { return age_; }
  const char* equipment() const { return equipment_; }
  const char* gliderId() const { return gliderId_; }

  const char* fieldLabel(Field field) const;
  const char* fieldText(Field field) const;
  size_t fieldMaxLength(Field field) const;
  bool appendChar(Field field, char value);
  bool backspace(Field field);
  bool clear(Field field);

 private:
  char name_[kNameSize] = {};
  char age_[kAgeSize] = {};
  char equipment_[kEquipmentSize] = {};
  char gliderId_[kGliderIdSize] = {};

  bool load();
  char* mutableField(Field field);
  static bool appendTo(char* text, size_t capacity, char value);
  static bool backspaceText(char* text);
  static void copySafe(char* dst, size_t capacity, const char* src);
};
