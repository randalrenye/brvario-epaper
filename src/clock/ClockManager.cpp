#include "clock/ClockManager.h"

#include <Wire.h>

#include "system/I2CBusLock.h"
#include "utilities.h"

namespace {

static constexpr int32_t kSaoPauloUtcOffsetSeconds = -3L * 3600L;
static constexpr uint32_t kRtcWriteMinIntervalMs = 60000UL;

bool isLeapYear(int year) {
  return ((year % 4) == 0 && (year % 100) != 0) || ((year % 400) == 0);
}

uint8_t daysInMonth(int year, int month) {
  static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && isLeapYear(year)) {
    return 29;
  }
  return days[month - 1];
}

void adjustDate(struct tm& timeInfo, int dayDelta) {
  int year = timeInfo.tm_year + 1900;
  int month = timeInfo.tm_mon + 1;
  int day = timeInfo.tm_mday + dayDelta;

  while (day < 1) {
    --month;
    if (month < 1) {
      month = 12;
      --year;
    }
    day += daysInMonth(year, month);
  }

  while (day > daysInMonth(year, month)) {
    day -= daysInMonth(year, month);
    ++month;
    if (month > 12) {
      month = 1;
      ++year;
    }
  }

  timeInfo.tm_year = year - 1900;
  timeInfo.tm_mon = month - 1;
  timeInfo.tm_mday = day;
}

}  // namespace

bool ClockManager::begin() {
  I2CBusGuard bus(pdMS_TO_TICKS(250));
  if (!bus.locked()) {
    Serial.println("Relogio RTC: barramento I2C ocupado na inicializacao.");
    return false;
  }

  Wire.begin(BOARD_SDA, BOARD_SCL);
  rtcReady_ = rtc_.begin(Wire, BOARD_SDA, BOARD_SCL);
  if (!rtcReady_) {
    Serial.println("Relogio RTC PCF8563 nao encontrado; usando NTP/GPS quando disponivel.");
    return false;
  }

  Serial.println("Relogio RTC PCF8563 iniciado.");
  readRtc();
  return true;
}

void ClockManager::updateFromNtpLocal(const struct tm& localTime) {
  if (!validDateTime(localTime)) {
    return;
  }

  setLocalTimeBase(secondsOfDay(localTime));
  writeRtcLocal(localTime, false);
}

void ClockManager::updateFromGpsUtc(const struct tm& utcTime) {
  if (!validDateTime(utcTime)) {
    return;
  }

  const struct tm localTime = convertUtcToLocal(utcTime);
  setLocalTimeBase(secondsOfDay(localTime));
  writeRtcLocal(localTime, false);
}

void ClockManager::updateFromGpsUtcSeconds(uint32_t utcSecondsOfDay) {
  int32_t localSeconds = static_cast<int32_t>(utcSecondsOfDay % 86400UL) + kSaoPauloUtcOffsetSeconds;
  while (localSeconds < 0) {
    localSeconds += 86400;
  }
  while (localSeconds >= 86400) {
    localSeconds -= 86400;
  }
  setLocalTimeBase(static_cast<uint32_t>(localSeconds));
}

void ClockManager::applyTo(VarioData& data) const {
  if (!timeValid_) {
    return;
  }

  const uint32_t elapsed = (millis() - clockSyncMs_) / 1000UL;
  data.timeOfDaySeconds = (localSecondsOfDay_ + elapsed) % 86400UL;
}

bool ClockManager::readRtc() {
  if (!rtcReady_) {
    return false;
  }

  I2CBusGuard bus(pdMS_TO_TICKS(100));
  if (!bus.locked()) {
    return false;
  }

  if (!rtc_.isClockIntegrityGuaranteed()) {
    Serial.println("Relogio RTC sem integridade garantida; aguardando NTP/GPS.");
    return false;
  }

  struct tm timeInfo = {};
  rtc_.getDateTime(&timeInfo);
  if (!validDateTime(timeInfo)) {
    Serial.println("Relogio RTC com data invalida; aguardando NTP/GPS.");
    return false;
  }

  setLocalTimeBase(secondsOfDay(timeInfo));
  Serial.printf("Relogio RTC carregado: %02d:%02d:%02d\n", timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
  return true;
}

void ClockManager::setLocalTimeBase(uint32_t secondsOfDayValue) {
  localSecondsOfDay_ = secondsOfDayValue % 86400UL;
  clockSyncMs_ = millis();
  timeValid_ = true;
}

void ClockManager::writeRtcLocal(const struct tm& localTime, bool force) {
  if (!rtcReady_) {
    return;
  }

  const uint32_t now = millis();
  if (!force && lastRtcWriteMs_ != 0 && now - lastRtcWriteMs_ < kRtcWriteMinIntervalMs) {
    return;
  }

  I2CBusGuard bus(pdMS_TO_TICKS(100));
  if (!bus.locked()) {
    return;
  }

  rtc_.setDateTime(localTime);
  lastRtcWriteMs_ = now;
  Serial.printf("Relogio RTC atualizado: %04d-%02d-%02d %02d:%02d:%02d\n",
                localTime.tm_year + 1900,
                localTime.tm_mon + 1,
                localTime.tm_mday,
                localTime.tm_hour,
                localTime.tm_min,
                localTime.tm_sec);
}

bool ClockManager::validDateTime(const struct tm& timeInfo) {
  const int year = timeInfo.tm_year + 1900;
  const int month = timeInfo.tm_mon + 1;
  if (year < 2024 || year > 2099) return false;
  if (month < 1 || month > 12) return false;
  if (timeInfo.tm_mday < 1 || timeInfo.tm_mday > daysInMonth(year, month)) return false;
  if (timeInfo.tm_hour < 0 || timeInfo.tm_hour > 23) return false;
  if (timeInfo.tm_min < 0 || timeInfo.tm_min > 59) return false;
  if (timeInfo.tm_sec < 0 || timeInfo.tm_sec > 59) return false;
  return true;
}

struct tm ClockManager::convertUtcToLocal(struct tm utcTime) {
  int32_t totalSeconds = utcTime.tm_hour * 3600L + utcTime.tm_min * 60L + utcTime.tm_sec + kSaoPauloUtcOffsetSeconds;
  while (totalSeconds < 0) {
    totalSeconds += 86400L;
    adjustDate(utcTime, -1);
  }
  while (totalSeconds >= 86400L) {
    totalSeconds -= 86400L;
    adjustDate(utcTime, 1);
  }

  utcTime.tm_hour = totalSeconds / 3600L;
  utcTime.tm_min = (totalSeconds % 3600L) / 60L;
  utcTime.tm_sec = totalSeconds % 60L;
  return utcTime;
}

uint32_t ClockManager::secondsOfDay(const struct tm& timeInfo) {
  return static_cast<uint32_t>(timeInfo.tm_hour) * 3600UL +
         static_cast<uint32_t>(timeInfo.tm_min) * 60UL +
         static_cast<uint32_t>(timeInfo.tm_sec);
}
