#include "tracklog/IgcLogger.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

uint32_t roundedUInt(float value) {
  if (value <= 0.0F) return 0;
  if (value >= 99999.0F) return 99999;
  return static_cast<uint32_t>(value + 0.5F);
}

void formatCoord(double degrees, bool latitude, char* out, size_t outSize) {
  const char hemi = latitude ? (degrees < 0.0 ? 'S' : 'N') : (degrees < 0.0 ? 'W' : 'E');
  double absDeg = fabs(degrees);
  uint16_t wholeDeg = static_cast<uint16_t>(absDeg);
  double minutes = (absDeg - static_cast<double>(wholeDeg)) * 60.0;
  uint32_t milliMinutes = static_cast<uint32_t>(minutes * 1000.0 + 0.5);
  if (milliMinutes >= 60000UL) {
    milliMinutes -= 60000UL;
    ++wholeDeg;
  }

  const uint16_t wholeMinutes = static_cast<uint16_t>(milliMinutes / 1000UL);
  const uint16_t minuteMillis = static_cast<uint16_t>(milliMinutes % 1000UL);
  if (latitude) {
    snprintf(out, outSize, "%02u%02u%03u%c", wholeDeg, wholeMinutes, minuteMillis, hemi);
  } else {
    snprintf(out, outSize, "%03u%02u%03u%c", wholeDeg, wholeMinutes, minuteMillis, hemi);
  }
}

}  // namespace

IgcLogger::IgcLogger() = default;

void IgcLogger::setFlightDate(uint8_t day, uint8_t month, uint16_t year) {
  if (day >= 1 && day <= 31) day_ = day;
  if (month >= 1 && month <= 12) month_ = month;
  yearTwoDigits_ = static_cast<uint8_t>(year % 100U);
}

bool IgcLogger::startFlight(fs::FS& fs,
                            const char* filepath,
                            const char* pilotName,
                            const char* gliderType,
                            const char* gliderId) {
  endFlight();
  if (!filepath || filepath[0] == '\0') {
    return false;
  }

  safeText(filepath, filepath_, sizeof(filepath_));
  fs.remove(filepath_);
  file_ = fs.open(filepath_, FILE_WRITE);
  if (!file_) {
    active_ = false;
    return false;
  }

  char pilot[40];
  char glider[40];
  char id[24];
  safeText(pilotName && pilotName[0] != '\0' ? pilotName : "PILOTO", pilot, sizeof(pilot));
  safeText(gliderType && gliderType[0] != '\0' ? gliderType : "PARAPENTE", glider, sizeof(glider));
  safeText(gliderId && gliderId[0] != '\0' ? gliderId : "BRVARIO", id, sizeof(id));

  char line[96];
  active_ = true;
  if (!writeLine("AXXXBRVARIO ESP32_LOGGER\r\n")) return false;
  snprintf(line, sizeof(line), "HFDTE%02u%02u%02u\r\n", day_, month_, yearTwoDigits_);
  if (!writeLine(line)) return false;
  snprintf(line, sizeof(line), "HFPLTPILOTINCHARGE:%s\r\n", pilot);
  if (!writeLine(line)) return false;
  snprintf(line, sizeof(line), "HFGTYGLIDERTYPE:%s\r\n", glider);
  if (!writeLine(line)) return false;
  snprintf(line, sizeof(line), "HFGIDGLIDERID:%s\r\n", id);
  if (!writeLine(line)) return false;
  if (!writeLine("HFDTM100GPSDATUM:WGS-84\r\n")) return false;
  if (!writeLine("HFFTYFRTYPE:BRVARIO E-PAPER\r\n")) return false;
  if (!writeLine("HFGPSGPS:UBLOX NEO-M8N\r\n")) return false;
  if (!writeLine("HFPRSPRESSALTSENSOR:BMP280\r\n")) return false;
  // J defines the optional fields that appear in K records. Bytes 8-11 hold signed vario in 0.1 m/s.
  if (!writeLine("J010811VAR\r\n")) return false;
  file_.flush();
  return true;
}

bool IgcLogger::logFix(uint32_t utcTimeSeconds,
                       double latDeg,
                       double lonDeg,
                       bool hasGpsFix,
                       float pressAltM,
                       float gpsAltM) {
  if (!active_ || !file_) {
    return false;
  }

  char utc[7];
  char lat[9];
  char lon[10];
  char line[48];
  formatUtc(utcTimeSeconds, utc, sizeof(utc));
  formatLatitude(latDeg, lat, sizeof(lat));
  formatLongitude(lonDeg, lon, sizeof(lon));
  const uint32_t pressAlt = clampAltitude(pressAltM);
  const uint32_t gpsAlt = clampAltitude(gpsAltM);
  snprintf(line,
           sizeof(line),
           "B%s%s%s%c%05lu%05lu\r\n",
           utc,
           lat,
           lon,
           hasGpsFix ? 'A' : 'V',
           static_cast<unsigned long>(pressAlt),
           static_cast<unsigned long>(gpsAlt));
  const bool ok = writeLine(line);
  file_.flush();
  return ok;
}

bool IgcLogger::logExtensionK(uint32_t utcTimeSeconds, float varioMs) {
  if (!active_ || !file_) {
    return false;
  }

  char utc[7];
  char vario[5];
  char line[20];
  formatUtc(utcTimeSeconds, utc, sizeof(utc));
  formatVarioTenths(varioMs, vario, sizeof(vario));
  snprintf(line, sizeof(line), "K%s%s\r\n", utc, vario);
  const bool ok = writeLine(line);
  file_.flush();
  return ok;
}

void IgcLogger::endFlight() {
  if (file_) {
    file_.flush();
    file_.close();
  }
  active_ = false;
}

bool IgcLogger::writeLine(const char* line) {
  if (!active_ || !file_ || !line) {
    active_ = false;
    return false;
  }
  const size_t len = strlen(line);
  const size_t written = file_.write(reinterpret_cast<const uint8_t*>(line), len);
  if (written != len) {
    active_ = false;
    return false;
  }
  return true;
}

void IgcLogger::safeText(const char* src, char* dst, size_t dstSize) {
  if (!dst || dstSize == 0) return;
  size_t out = 0;
  if (src) {
    for (size_t i = 0; src[i] != '\0' && out + 1 < dstSize; ++i) {
      const char c = src[i];
      if (c >= 32 && c <= 126) {
        dst[out++] = c;
      }
    }
  }
  dst[out] = '\0';
}

uint32_t IgcLogger::clampAltitude(float altitudeM) {
  return roundedUInt(altitudeM);
}

void IgcLogger::formatUtc(uint32_t utcTimeSeconds, char* out, size_t outSize) {
  utcTimeSeconds %= 86400UL;
  const uint32_t hour = utcTimeSeconds / 3600UL;
  const uint32_t minute = (utcTimeSeconds / 60UL) % 60UL;
  const uint32_t second = utcTimeSeconds % 60UL;
  snprintf(out, outSize, "%02lu%02lu%02lu", static_cast<unsigned long>(hour), static_cast<unsigned long>(minute),
           static_cast<unsigned long>(second));
}

void IgcLogger::formatLatitude(double latDeg, char* out, size_t outSize) {
  formatCoord(latDeg, true, out, outSize);
}

void IgcLogger::formatLongitude(double lonDeg, char* out, size_t outSize) {
  formatCoord(lonDeg, false, out, outSize);
}

void IgcLogger::formatVarioTenths(float varioMs, char* out, size_t outSize) {
  int16_t tenths = static_cast<int16_t>(varioMs * 10.0F + (varioMs >= 0.0F ? 0.5F : -0.5F));
  if (tenths > 999) tenths = 999;
  if (tenths < -999) tenths = -999;
  snprintf(out, outSize, "%c%03u", tenths < 0 ? '-' : '+', static_cast<unsigned>(tenths < 0 ? -tenths : tenths));
}
