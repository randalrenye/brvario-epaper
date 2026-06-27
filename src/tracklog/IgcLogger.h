#pragma once

#include <Arduino.h>
#include <FS.h>
#include <stdint.h>

class IgcLogger {
 public:
  IgcLogger();

  void setFlightDate(uint8_t day, uint8_t month, uint16_t year);
  bool startFlight(fs::FS& fs, const char* filepath, const char* pilotName, const char* gliderType, const char* gliderId);
  bool logFix(uint32_t utcTimeSeconds, double latDeg, double lonDeg, bool hasGpsFix, float pressAltM, float gpsAltM);
  bool logExtensionK(uint32_t utcTimeSeconds, float varioMs);
  void endFlight();

  bool isActive() const { return active_; }
  const char* filepath() const { return filepath_; }

 private:
  static constexpr size_t kPathSize = 64;

  fs::File file_;
  char filepath_[kPathSize] = {};
  uint8_t day_ = 1;
  uint8_t month_ = 1;
  uint8_t yearTwoDigits_ = 0;
  bool active_ = false;

  bool writeLine(const char* line);
  static void safeText(const char* src, char* dst, size_t dstSize);
  static uint32_t clampAltitude(float altitudeM);
  static void formatUtc(uint32_t utcTimeSeconds, char* out, size_t outSize);
  static void formatLatitude(double latDeg, char* out, size_t outSize);
  static void formatLongitude(double lonDeg, char* out, size_t outSize);
  static void formatVarioTenths(float varioMs, char* out, size_t outSize);
};
