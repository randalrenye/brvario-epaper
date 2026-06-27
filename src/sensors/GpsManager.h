#pragma once

#include <Arduino.h>
#include <time.h>

#include "data/VarioData.h"

class GpsManager {
 public:
  bool begin();
  void end();
  void poll();
  void applyTo(VarioData& data) const;
  void printStatus(Stream& out) const;

  bool isReady() const { return ready_; }
  bool hasFix() const { return fixValid_; }
  bool hasReceivedData() const { return lastSentenceMs_ != 0; }
  uint32_t lastNavigationMs() const { return lastNavigationMs_; }
  uint32_t lastSentenceAgeMs() const;
  uint32_t lastFixAgeMs() const;
  bool getUtcTimeOfDay(uint32_t& seconds) const;
  bool getUtcDateTime(struct tm& utcTime) const;

 private:
  static constexpr size_t kLineBufferSize = 128;

  char lineBuffer_[kLineBufferSize] = {};
  size_t lineLength_ = 0;
  bool ready_ = false;
  bool fixValid_ = false;
  bool hasTime_ = false;
  bool hasDate_ = false;
  bool hasLocation_ = false;
  bool altitudeValid_ = false;
  bool groundReferenceSet_ = false;
  uint8_t satellites_ = 0;
  float hdop_ = 99.9F;
  uint32_t timeOfDaySeconds_ = 0;
  uint32_t lastSentenceMs_ = 0;
  uint32_t lastFixMs_ = 0;
  uint32_t lastNavigationMs_ = 0;
  uint16_t year_ = 0;
  uint8_t month_ = 0;
  uint8_t day_ = 0;
  float latitudeDeg_ = 0.0F;
  float longitudeDeg_ = 0.0F;
  float altitudeM_ = 0.0F;
  float groundReferenceM_ = 0.0F;
  float groundSpeedKmh_ = 0.0F;
  float courseDeg_ = 0.0F;

  void configureForVario();
  void setNmeaMessageRate(uint8_t messageId, uint8_t rate);
  void sendUbx(uint8_t messageClass, uint8_t messageId, const uint8_t* payload, uint16_t length);
  void parseLine(char* line);
  void parseGga(char* line);
  void parseRmc(char* line);
  bool validChecksum(const char* line) const;
  uint8_t splitFields(char* line, char* fields[], uint8_t maxFields) const;
  bool parseTime(const char* text, uint32_t& seconds) const;
  bool parseDate(const char* text, uint16_t& year, uint8_t& month, uint8_t& day) const;
  bool parseCoordinate(const char* value, const char* hemisphere, float& degrees) const;
};
