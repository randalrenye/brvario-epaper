#pragma once

#include <Arduino.h>
#include <time.h>

#include "data/VarioData.h"
#include "time/pcf8563/SensorPCF8563.hpp"

class ClockManager {
 public:
  bool begin();
  void updateFromNtpLocal(const struct tm& localTime);
  void updateFromGpsUtc(const struct tm& utcTime);
  void updateFromGpsUtcSeconds(uint32_t utcSecondsOfDay);
  void applyTo(VarioData& data) const;
  bool hasTime() const { return timeValid_; }
  bool rtcReady() const { return rtcReady_; }

 private:
  SensorPCF8563 rtc_;
  bool rtcReady_ = false;
  bool timeValid_ = false;
  uint32_t clockSyncMs_ = 0;
  uint32_t localSecondsOfDay_ = 0;
  uint32_t lastRtcWriteMs_ = 0;

  bool readRtc();
  void setLocalTimeBase(uint32_t secondsOfDay);
  void writeRtcLocal(const struct tm& localTime, bool force);
  static bool validDateTime(const struct tm& timeInfo);
  static struct tm convertUtcToLocal(struct tm utcTime);
  static uint32_t secondsOfDay(const struct tm& timeInfo);
};
