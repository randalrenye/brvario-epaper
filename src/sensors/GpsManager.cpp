#include "sensors/GpsManager.h"

#include <stdlib.h>
#include <string.h>

#include "config/HardwareConfig.h"

namespace {

static constexpr uint32_t kGpsSignalTimeoutMs = 3000UL;
static constexpr float kAglGroundDeadbandM = 3.0F;

}  // namespace

bool GpsManager::begin() {
  Serial1.begin(HardwareConfig::kGpsBaud, SERIAL_8N1, HardwareConfig::kGpsRxPin, HardwareConfig::kGpsTxPin);
  ready_ = true;
  lineLength_ = 0;
  Serial.printf("GPS UART iniciado: RX=%d TX=%d baud=%lu\n",
                HardwareConfig::kGpsRxPin,
                HardwareConfig::kGpsTxPin,
                static_cast<unsigned long>(HardwareConfig::kGpsBaud));
  configureForVario();
  return true;
}

void GpsManager::end() {
  Serial1.end();
  pinMode(HardwareConfig::kGpsRxPin, INPUT);
  pinMode(HardwareConfig::kGpsTxPin, INPUT);
  ready_ = false;
}

void GpsManager::poll() {
  if (!ready_) return;

  while (Serial1.available() > 0) {
    const char c = static_cast<char>(Serial1.read());
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      if (lineLength_ > 0) {
        lineBuffer_[lineLength_] = '\0';
        parseLine(lineBuffer_);
        lineLength_ = 0;
      }
      continue;
    }
    if (lineLength_ < kLineBufferSize - 1) {
      lineBuffer_[lineLength_++] = c;
    } else {
      lineLength_ = 0;
    }
  }
}

void GpsManager::applyTo(VarioData& data) const {
  const bool signalFresh = lastSentenceMs_ != 0 && millis() - lastSentenceMs_ <= kGpsSignalTimeoutMs;
  const bool activeFix = signalFresh && fixValid_;

  data.gpsFix = activeFix;
  data.satellites = signalFresh ? satellites_ : 0;
  data.gpsHdop = signalFresh ? hdop_ : 99.9F;
  data.gpsLastSentenceAgeMs = lastSentenceAgeMs();
  data.gpsLastFixAgeMs = lastFixAgeMs();
  if (!ready_) {
    data.gpsStatus = GpsSensorStatus::Off;
  } else if (lastSentenceMs_ == 0) {
    data.gpsStatus = GpsSensorStatus::NoData;
  } else if (!signalFresh) {
    data.gpsStatus = GpsSensorStatus::StaleData;
  } else if (!fixValid_) {
    data.gpsStatus = GpsSensorStatus::NoFix;
  } else {
    data.gpsStatus = GpsSensorStatus::Fix;
  }
  data.timeOfDaySeconds = 0;

  if (signalFresh && hasTime_) {
    data.timeOfDaySeconds = timeOfDaySeconds_;
  }

  if (!activeFix) {
    data.latitudeDeg = 0.0F;
    data.longitudeDeg = 0.0F;
    data.altitudeGpsM = 0.0F;
    data.altitudeGpsAglM = 0.0F;
    data.altitudeAglM = 0.0F;
    data.groundSpeedKmh = 0.0F;
    data.courseDeg = 0.0F;
    return;
  }

  if (hasLocation_) {
    data.latitudeDeg = latitudeDeg_;
    data.longitudeDeg = longitudeDeg_;
  } else {
    data.latitudeDeg = 0.0F;
    data.longitudeDeg = 0.0F;
  }

  if (altitudeValid_) {
    data.altitudeGpsM = altitudeM_;
    float aglM = groundReferenceSet_ ? altitudeM_ - groundReferenceM_ : 0.0F;
    if (aglM < kAglGroundDeadbandM) {
      aglM = 0.0F;
    }
    data.altitudeGpsAglM = aglM;
    data.altitudeAglM = aglM;
  } else {
    data.altitudeGpsM = 0.0F;
    data.altitudeGpsAglM = 0.0F;
    data.altitudeAglM = 0.0F;
  }

  data.groundSpeedKmh = groundSpeedKmh_;
  data.courseDeg = courseDeg_;
}

void GpsManager::printStatus(Stream& out) const {
  out.print("GPS: ");
  out.print(fixValid_ ? "FIX" : "SEM FIX");
  out.print(" sats=");
  out.print(satellites_);
  out.print(" hdop=");
  out.print(hdop_, 1);
  out.print(" lat=");
  out.print(latitudeDeg_, 6);
  out.print(" lon=");
  out.print(longitudeDeg_, 6);
  out.print(" alt=");
  out.print(altitudeM_, 1);
  out.print("m spd=");
  out.print(groundSpeedKmh_, 1);
  out.print("km/h course=");
  out.print(courseDeg_, 0);
  out.print(" agl_base=");
  out.print(groundReferenceSet_ ? groundReferenceM_ : 0.0F, 1);
  out.print(" last=");
  out.print(millis() - lastSentenceMs_);
  out.println("ms");
}

uint32_t GpsManager::lastSentenceAgeMs() const {
  if (lastSentenceMs_ == 0) {
    return UINT32_MAX;
  }
  return millis() - lastSentenceMs_;
}

uint32_t GpsManager::lastFixAgeMs() const {
  if (lastFixMs_ == 0) {
    return UINT32_MAX;
  }
  return millis() - lastFixMs_;
}

bool GpsManager::getUtcTimeOfDay(uint32_t& seconds) const {
  const bool signalFresh = lastSentenceMs_ != 0 && millis() - lastSentenceMs_ <= kGpsSignalTimeoutMs;
  if (!signalFresh || !hasTime_) {
    return false;
  }
  seconds = timeOfDaySeconds_;
  return true;
}

bool GpsManager::getUtcDateTime(struct tm& utcTime) const {
  uint32_t seconds = 0;
  if (!getUtcTimeOfDay(seconds) || !hasDate_) {
    return false;
  }

  memset(&utcTime, 0, sizeof(utcTime));
  utcTime.tm_year = static_cast<int>(year_) - 1900;
  utcTime.tm_mon = static_cast<int>(month_) - 1;
  utcTime.tm_mday = day_;
  utcTime.tm_hour = seconds / 3600UL;
  utcTime.tm_min = (seconds % 3600UL) / 60UL;
  utcTime.tm_sec = seconds % 60UL;
  return true;
}

void GpsManager::configureForVario() {
  // Keep the UART at 9600, but reduce NMEA traffic and ask u-blox modules for 5 Hz.
  // NEO-M8N accepts these UBX-CFG messages; other GPS modules will simply ignore them.
  setNmeaMessageRate(0x00, 1);  // GGA: fix, satellites, GPS altitude
  setNmeaMessageRate(0x04, 1);  // RMC: time, position, speed, course
  setNmeaMessageRate(0x01, 0);  // GLL
  setNmeaMessageRate(0x02, 0);  // GSA
  setNmeaMessageRate(0x03, 0);  // GSV
  setNmeaMessageRate(0x05, 0);  // VTG

  const uint8_t ratePayload[] = {
      0xC8, 0x00,  // measurement rate: 200 ms
      0x01, 0x00,  // navigation rate: every measurement
      0x01, 0x00,  // time reference: GPS time
  };
  sendUbx(0x06, 0x08, ratePayload, sizeof(ratePayload));
  Serial.println("GPS configurado para NMEA GGA/RMC em ate 5 Hz quando suportado.");
}

void GpsManager::setNmeaMessageRate(uint8_t messageId, uint8_t rate) {
  const uint8_t payload[] = {
      0xF0,
      messageId,
      rate,
  };
  sendUbx(0x06, 0x01, payload, sizeof(payload));
}

void GpsManager::sendUbx(uint8_t messageClass, uint8_t messageId, const uint8_t* payload, uint16_t length) {
  uint8_t ckA = 0;
  uint8_t ckB = 0;

  auto addChecksum = [&](uint8_t value) {
    ckA = static_cast<uint8_t>(ckA + value);
    ckB = static_cast<uint8_t>(ckB + ckA);
  };

  Serial1.write(0xB5);
  Serial1.write(0x62);
  Serial1.write(messageClass);
  Serial1.write(messageId);
  Serial1.write(static_cast<uint8_t>(length & 0xFF));
  Serial1.write(static_cast<uint8_t>(length >> 8));

  addChecksum(messageClass);
  addChecksum(messageId);
  addChecksum(static_cast<uint8_t>(length & 0xFF));
  addChecksum(static_cast<uint8_t>(length >> 8));

  for (uint16_t i = 0; i < length; ++i) {
    const uint8_t value = payload ? payload[i] : 0;
    Serial1.write(value);
    addChecksum(value);
  }

  Serial1.write(ckA);
  Serial1.write(ckB);
  Serial1.flush();
  delay(30);
}

void GpsManager::parseLine(char* line) {
  if (!line || line[0] != '$') return;
  if (!validChecksum(line)) return;

  lastSentenceMs_ = millis();
  if (strstr(line, "GGA,") != nullptr) {
    parseGga(line);
  } else if (strstr(line, "RMC,") != nullptr) {
    parseRmc(line);
  }
}

void GpsManager::parseGga(char* line) {
  char* checksum = strchr(line, '*');
  if (checksum) *checksum = '\0';

  char* fields[16] = {};
  const uint8_t count = splitFields(line, fields, 16);
  if (count < 10) return;

  uint32_t seconds = 0;
  if (parseTime(fields[1], seconds)) {
    timeOfDaySeconds_ = seconds;
    hasTime_ = true;
  }

  const int fixQuality = atoi(fields[6]);
  satellites_ = static_cast<uint8_t>(atoi(fields[7]));
  hdop_ = fields[8] && fields[8][0] != '\0' ? static_cast<float>(atof(fields[8])) : 99.9F;
  fixValid_ = fixQuality > 0;

  float latitude = 0.0F;
  float longitude = 0.0F;
  if (fixValid_ && parseCoordinate(fields[2], fields[3], latitude) && parseCoordinate(fields[4], fields[5], longitude)) {
    latitudeDeg_ = latitude;
    longitudeDeg_ = longitude;
    hasLocation_ = true;
  }

  altitudeValid_ = fixValid_ && fields[9] && fields[9][0] != '\0';
  if (altitudeValid_) {
    altitudeM_ = static_cast<float>(atof(fields[9]));
    if (!groundReferenceSet_) {
      groundReferenceM_ = altitudeM_;
      groundReferenceSet_ = true;
    }
  }
  if (fixValid_) {
    lastFixMs_ = millis();
  }
}

void GpsManager::parseRmc(char* line) {
  char* checksum = strchr(line, '*');
  if (checksum) *checksum = '\0';

  char* fields[16] = {};
  const uint8_t count = splitFields(line, fields, 16);
  if (count < 10) return;

  uint32_t seconds = 0;
  if (parseTime(fields[1], seconds)) {
    timeOfDaySeconds_ = seconds;
    hasTime_ = true;
  }

  const bool rmcValid = fields[2] && fields[2][0] == 'A';
  if (!rmcValid) {
    fixValid_ = false;
    groundSpeedKmh_ = 0.0F;
    courseDeg_ = 0.0F;
    return;
  }

  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  if (parseDate(fields[9], year, month, day)) {
    year_ = year;
    month_ = month;
    day_ = day;
    hasDate_ = true;
  }

  float latitude = 0.0F;
  float longitude = 0.0F;
  if (parseCoordinate(fields[3], fields[4], latitude) && parseCoordinate(fields[5], fields[6], longitude)) {
    latitudeDeg_ = latitude;
    longitudeDeg_ = longitude;
    hasLocation_ = true;
  }

  const float speedKnots = static_cast<float>(atof(fields[7]));
  groundSpeedKmh_ = speedKnots * 1.852F;
  courseDeg_ = static_cast<float>(atof(fields[8]));
  fixValid_ = true;
  lastFixMs_ = millis();
  lastNavigationMs_ = lastFixMs_;
}

bool GpsManager::validChecksum(const char* line) const {
  const char* star = strchr(line, '*');
  if (!star) return true;
  if (star - line < 2 || strlen(star) < 3) return false;

  uint8_t checksum = 0;
  for (const char* p = line + 1; p < star; ++p) {
    checksum ^= static_cast<uint8_t>(*p);
  }

  char expectedText[3] = {star[1], star[2], '\0'};
  const uint8_t expected = static_cast<uint8_t>(strtoul(expectedText, nullptr, 16));
  return checksum == expected;
}

uint8_t GpsManager::splitFields(char* line, char* fields[], uint8_t maxFields) const {
  uint8_t count = 0;
  char* cursor = line;
  while (count < maxFields) {
    fields[count++] = cursor;
    char* comma = strchr(cursor, ',');
    if (!comma) break;
    *comma = '\0';
    cursor = comma + 1;
  }
  return count;
}

bool GpsManager::parseTime(const char* text, uint32_t& seconds) const {
  if (!text || strlen(text) < 6) return false;

  const uint32_t hours = static_cast<uint32_t>((text[0] - '0') * 10 + (text[1] - '0'));
  const uint32_t minutes = static_cast<uint32_t>((text[2] - '0') * 10 + (text[3] - '0'));
  const uint32_t secs = static_cast<uint32_t>((text[4] - '0') * 10 + (text[5] - '0'));
  if (hours > 23 || minutes > 59 || secs > 59) return false;

  seconds = hours * 3600UL + minutes * 60UL + secs;
  return true;
}

bool GpsManager::parseDate(const char* text, uint16_t& year, uint8_t& month, uint8_t& day) const {
  if (!text || strlen(text) < 6) return false;
  for (uint8_t i = 0; i < 6; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
  }

  day = static_cast<uint8_t>((text[0] - '0') * 10 + (text[1] - '0'));
  month = static_cast<uint8_t>((text[2] - '0') * 10 + (text[3] - '0'));
  const uint8_t yearTwoDigits = static_cast<uint8_t>((text[4] - '0') * 10 + (text[5] - '0'));
  year = static_cast<uint16_t>(yearTwoDigits >= 80 ? 1900 + yearTwoDigits : 2000 + yearTwoDigits);

  if (year < 2024 || year > 2099) return false;
  if (month < 1 || month > 12) return false;
  if (day < 1 || day > 31) return false;
  return true;
}

bool GpsManager::parseCoordinate(const char* value, const char* hemisphere, float& degrees) const {
  if (!value || !hemisphere || value[0] == '\0' || hemisphere[0] == '\0') return false;

  const float raw = static_cast<float>(atof(value));
  const int deg = static_cast<int>(raw / 100.0F);
  const float minutes = raw - static_cast<float>(deg * 100);
  degrees = static_cast<float>(deg) + minutes / 60.0F;

  if (hemisphere[0] == 'S' || hemisphere[0] == 'W') {
    degrees = -degrees;
  }
  return true;
}
