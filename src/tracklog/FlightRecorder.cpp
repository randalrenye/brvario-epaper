#include "tracklog/FlightRecorder.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

static constexpr float kEarthRadiusM = 6371000.0F;
static constexpr int32_t kDisplayUtcOffsetSeconds = -3L * 3600L;
static constexpr char kSdTracklogPrefix[] = "sd:";

float degToRad(double deg) {
  return static_cast<float>(deg * 0.017453292519943295);
}

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

void adjustDateParts(int& year, int& month, int& day, int dayDelta) {
  day += dayDelta;
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
}

uint8_t parseTwoDigits(const char* text) {
  if (!text || text[0] < '0' || text[0] > '9' || text[1] < '0' || text[1] > '9') {
    return 0;
  }
  return static_cast<uint8_t>((text[0] - '0') * 10 + (text[1] - '0'));
}

uint16_t parseFourDigits(const char* text) {
  if (!text) return 0;
  uint16_t value = 0;
  for (uint8_t i = 0; i < 4; ++i) {
    if (text[i] < '0' || text[i] > '9') return 0;
    value = static_cast<uint16_t>(value * 10U + static_cast<uint16_t>(text[i] - '0'));
  }
  return value;
}

void normalizeIgcPath(const char* input, char* out, size_t outSize) {
  if (!out || outSize == 0) {
    return;
  }
  out[0] = '\0';
  if (!input || input[0] == '\0') {
    return;
  }
  if (input[0] == '/') {
    snprintf(out, outSize, "%s", input);
    return;
  }
  if (strncmp(input, "igc/", 4) == 0 || strncmp(input, "IGC/", 4) == 0) {
    snprintf(out, outSize, "/%s", input);
    return;
  }
  snprintf(out, outSize, "/igc/%s", input);
}

}  // namespace

FlightRecorder::FlightRecorder() = default;

bool FlightRecorder::begin(fs::FS& fs, PilotProfileConfig& profile) {
  fs_ = &fs;
  profile_ = &profile;
  storageReady_ = true;
  state_ = State::WaitingGps;
  currentPath_[0] = '\0';
  completedPath_[0] = '\0';
  completedFlightPending_ = false;
  takeoffDetectedPending_ = false;
  activeLogFs_ = fs_;
  activeLogExternal_ = false;
  backfillNext_ = 0;
  backfillCount_ = 0;
  lastSampleUtcSecond_ = UINT32_MAX;
  takeoffCandidateMs_ = 0;
  takeoffLatDeg_ = 0.0;
  takeoffLonDeg_ = 0.0;
  resetLandingCandidate();
  noFixLandingCandidateMs_ = 0;
  flightStartMs_ = 0;
  flightElapsedSeconds_ = 0;
  filteredGroundSpeedKmh_ = 0.0F;
  speedFilterValid_ = false;
  if (!fs_->exists("/igc")) {
    fs_->mkdir("/igc");
  }
  return true;
}

void FlightRecorder::attachArchiveStorage(fs::FS* fs, const char* directory) {
  archiveFs_ = fs;
  archiveReady_ = false;
  archiveDir_[0] = '\0';
  if (!fs || !directory || directory[0] != '/') {
    return;
  }

  snprintf(archiveDir_, sizeof(archiveDir_), "%s", directory);
  archiveReady_ = true;
}

void FlightRecorder::update(VarioData& data, const struct tm& utcDateTime, bool hasUtcDateTime) {
  const uint32_t now = millis();
  if (!storageReady_ || !fs_ || !profile_) {
    state_ = State::StorageError;
    data.trackingEnabled = false;
    return;
  }

  if (!hasUtcDateTime || !data.gpsFix) {
    if (state_ != State::Recording) {
      state_ = State::WaitingGps;
      data.trackingEnabled = false;
    } else {
      updateElapsed(data, now);
      const bool calmSensor = data.sensorDataValid && fabsf(data.varioMs) <= kLandingCalmVarioMs;
      if (calmSensor && (!data.gpsFix || data.groundSpeedKmh <= 1.0F)) {
        if (noFixLandingCandidateMs_ == 0) {
          noFixLandingCandidateMs_ = now;
          Serial.println("Tracklog: candidato de pouso sem fix GPS iniciado.");
        } else if (now - noFixLandingCandidateMs_ >= kNoFixLandingConfirmMs) {
          Serial.println("Tracklog: pouso detectado sem fix GPS por estabilidade no solo.");
          endFlight(data);
        }
      } else {
        noFixLandingCandidateMs_ = 0;
      }
    }
    return;
  }

  const FlightSample sample = makeSample(data, utcDateTime, now);
  updateSpeedFilter(sample.groundSpeedKmh);
  if (sample.utcSeconds != lastSampleUtcSecond_) {
    pushBackfill(sample);
    lastSampleUtcSecond_ = sample.utcSeconds;
    if (state_ == State::Recording) {
      logSample(sample);
    }
  }

  if (state_ != State::Recording) {
    data.trackingEnabled = false;
    if (state_ == State::WaitingGps) {
      state_ = State::WaitingTakeoff;
    }
    if (detectTakeoff(sample) && startFlight(sample, utcDateTime)) {
      data.trackingEnabled = true;
      data.elapsedSeconds = 0;
      flightElapsedSeconds_ = 0;
      return;
    }
    data.elapsedSeconds = flightElapsedSeconds_;
    return;
  }

  updateElapsed(data, now);
  if (detectLanding(sample)) {
    endFlight(data);
  }
}

void FlightRecorder::endFlight(VarioData& data) {
  const bool shortFlight = flightStartMs_ != 0 && millis() - flightStartMs_ < kMinimumRecordedFlightMs;
  if (logger_.isActive()) {
    logger_.endFlight();
  }
  fs::FS* logFs = activeLogFs_ ? activeLogFs_ : fs_;
  if (shortFlight && currentPath_[0] != '\0' && logFs) {
    logFs->remove(currentPath_);
    Serial.print("Tracklog: voo menor que 60s descartado: ");
    Serial.println(currentPath_);
  } else if (currentPath_[0] != '\0') {
    if (activeLogExternal_) {
      snprintf(completedPath_, sizeof(completedPath_), "%s%s", kSdTracklogPrefix, currentPath_);
    } else {
      snprintf(completedPath_, sizeof(completedPath_), "%s", currentPath_);
    }
    completedFlightPending_ = true;
  }
  data.trackingEnabled = false;
  flightElapsedSeconds_ = 0;
  data.elapsedSeconds = 0;
  state_ = State::Landed;
  takeoffCandidateMs_ = 0;
  noFixLandingCandidateMs_ = 0;
  resetLandingCandidate();
  speedFilterValid_ = false;
  activeLogFs_ = fs_;
  activeLogExternal_ = false;
}

const char* FlightRecorder::statusText() const {
  switch (state_) {
    case State::WaitingGps:
      return "AGUARDANDO GPS";
    case State::WaitingTakeoff:
      return "AGUARDANDO DECOLAGEM";
    case State::Recording:
      return "GRAVANDO IGC";
    case State::Landed:
      return "POUSO DETECTADO";
    case State::StorageError:
      return "ERRO MEMORIA";
    case State::StorageFull:
      return "MEMORIA CHEIA";
    default:
      return "DESCONHECIDO";
  }
}

uint16_t FlightRecorder::tracklogCount() const {
  if (!storageReady_ || !fs_) {
    return 0;
  }
  return static_cast<uint16_t>(countTracklogsIn(fs_, "/igc") +
                               (archiveReady_ ? countTracklogsIn(archiveFs_, archiveDir_) : 0));
}

bool FlightRecorder::tracklogEntry(uint16_t newestFirstIndex, TracklogEntry& entry) const {
  TracklogEntry single[1] = {};
  uint8_t entryCount = 0;
  uint16_t totalCount = 0;
  uint32_t usedBytes = 0;
  if (!tracklogPage(newestFirstIndex, 1, single, entryCount, totalCount, usedBytes) || entryCount == 0) {
    entry = {};
    return false;
  }
  entry = single[0];
  return true;
}

bool FlightRecorder::tracklogPage(uint16_t page,
                                  uint8_t pageSize,
                                  TracklogEntry* entries,
                                  uint8_t& entryCount,
                                  uint16_t& totalCount,
                                  uint32_t& usedBytes) const {
  entryCount = 0;
  totalCount = 0;
  usedBytes = 0;
  if (!storageReady_ || !fs_ || !entries || pageSize == 0) {
    return false;
  }

  const uint16_t firstIndex = static_cast<uint16_t>(page * pageSize);
  uint16_t keepCount = static_cast<uint16_t>(firstIndex + pageSize);
  if (keepCount > kTracklogListMaxEntries) {
    keepCount = kTracklogListMaxEntries;
  }

  for (uint8_t i = 0; i < kTracklogListMaxEntries; ++i) {
    tracklogListScratch_[i] = {};
  }

  uint8_t rankedCount = 0;
  const auto compareCandidate = [](const char* name, bool external, const TracklogEntry& existing) -> int {
    const int byName = strcmp(name, existing.name);
    if (byName != 0) return byName;
    if (external == existing.external) return 0;
    return external ? -1 : 1;  // Keep LOCAL before SD when names match.
  };

  const auto scanSource = [&](fs::FS* sourceFs, const char* directory, bool external, bool includeInLocalUsed) {
    if (!sourceFs || !directory || directory[0] == '\0') {
      return;
    }

    File root = sourceFs->open(directory);
    if (!root || !root.isDirectory()) {
      if (root) root.close();
      return;
    }

    File file = root.openNextFile();
    while (file) {
      if (!file.isDirectory() && isIgcFilename(file.name())) {
        ++totalCount;
        const uint32_t fileSize = static_cast<uint32_t>(file.size());
        if (includeInLocalUsed) {
          usedBytes += fileSize;
        }

        char candidateName[64] = {};
        copyFileName(file.name(), candidateName, sizeof(candidateName));
        if (keepCount > 0 &&
            (rankedCount < keepCount || compareCandidate(candidateName, external, tracklogListScratch_[rankedCount - 1]) > 0)) {
          uint8_t insertAt = rankedCount;
          while (insertAt > 0 && compareCandidate(candidateName, external, tracklogListScratch_[insertAt - 1]) > 0) {
            --insertAt;
          }

          if (insertAt < keepCount) {
            const uint8_t last = rankedCount < keepCount ? rankedCount : static_cast<uint8_t>(keepCount - 1);
            for (uint8_t i = last; i > insertAt; --i) {
              tracklogListScratch_[i] = tracklogListScratch_[i - 1];
            }

            if (external) {
              snprintf(tracklogListScratch_[insertAt].path,
                       sizeof(tracklogListScratch_[insertAt].path),
                       "%s%s/%s",
                       kSdTracklogPrefix,
                       directory,
                       candidateName);
              snprintf(tracklogListScratch_[insertAt].location, sizeof(tracklogListScratch_[insertAt].location), "SD");
            } else {
              normalizeIgcPath(candidateName, tracklogListScratch_[insertAt].path, sizeof(tracklogListScratch_[insertAt].path));
              snprintf(tracklogListScratch_[insertAt].location, sizeof(tracklogListScratch_[insertAt].location), "LOCAL");
            }
            snprintf(tracklogListScratch_[insertAt].name, sizeof(tracklogListScratch_[insertAt].name), "%s", candidateName);
            tracklogListScratch_[insertAt].sizeBytes = fileSize;
            tracklogListScratch_[insertAt].external = external;
            if (rankedCount < keepCount) {
              ++rankedCount;
            }
          }
        }
      }
      file.close();
      file = root.openNextFile();
    }
    root.close();
  };

  scanSource(fs_, "/igc", false, true);
  if (archiveReady_) {
    scanSource(archiveFs_, archiveDir_, true, false);
  }

  if (firstIndex >= rankedCount) {
    return true;
  }

  const uint8_t available = static_cast<uint8_t>(rankedCount - firstIndex);
  entryCount = available < pageSize ? available : pageSize;
  for (uint8_t i = 0; i < entryCount; ++i) {
    entries[i] = tracklogListScratch_[firstIndex + i];
    formatEntryDateTime(entries[i].name,
                        entries[i].displayDate,
                        sizeof(entries[i].displayDate),
                        entries[i].displayTime,
                        sizeof(entries[i].displayTime));
    if (entries[i].displayTime[0] == '\0') {
      TracklogStats stats;
      if (tracklogStats(entries[i].path, stats)) {
        snprintf(entries[i].displayTime,
                 sizeof(entries[i].displayTime),
                 "%02lu:%02lu",
                 static_cast<unsigned long>(stats.startUtcSeconds / 3600UL),
                 static_cast<unsigned long>((stats.startUtcSeconds / 60UL) % 60UL));
      }
    }
  }
  return true;
}

bool FlightRecorder::readTracklogChunk(const char* filepath,
                                       uint32_t offset,
                                       uint8_t* buffer,
                                       size_t bufferSize,
                                       size_t& bytesRead,
                                       uint32_t& fileSize) const {
  bytesRead = 0;
  fileSize = 0;
  if (!storageReady_ || !fs_ || !filepath || filepath[0] == '\0' || !buffer || bufferSize == 0) {
    return false;
  }

  fs::FS* sourceFs = nullptr;
  bool external = false;
  char normalizedPath[96];
  if (!resolveTracklogPath(filepath, sourceFs, normalizedPath, sizeof(normalizedPath), external)) {
    return false;
  }

  File file = sourceFs->open(normalizedPath, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    return false;
  }

  fileSize = static_cast<uint32_t>(file.size());
  if (offset >= fileSize) {
    file.close();
    return true;
  }

  if (!file.seek(offset)) {
    file.close();
    return false;
  }
  bytesRead = file.read(buffer, bufferSize);
  file.close();
  return true;
}

bool FlightRecorder::tracklogStats(const char* filepath, TracklogStats& stats) const {
  memset(&stats, 0, sizeof(stats));
  if (!storageReady_ || !fs_ || !filepath || filepath[0] == '\0') {
    return false;
  }

  fs::FS* sourceFs = nullptr;
  bool external = false;
  char normalizedPath[96];
  if (!resolveTracklogPath(filepath, sourceFs, normalizedPath, sizeof(normalizedPath), external)) {
    return false;
  }

  File file = sourceFs->open(normalizedPath, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    Serial.print("Tracklog: falha ao abrir IGC para estatistica: ");
    Serial.println(normalizedPath[0] != '\0' ? normalizedPath : filepath);
    return false;
  }

  if (external) {
    snprintf(stats.path, sizeof(stats.path), "%s%s", kSdTracklogPrefix, normalizedPath);
    snprintf(stats.location, sizeof(stats.location), "SD");
  } else {
    snprintf(stats.path, sizeof(stats.path), "%s", normalizedPath);
    snprintf(stats.location, sizeof(stats.location), "LOCAL");
  }
  copyFileName(stats.path, stats.name, sizeof(stats.name));
  stats.sizeBytes = static_cast<uint32_t>(file.size());
  stats.minVarioMs = 0.0F;

  char line[120] = {};
  size_t lineLen = 0;
  bool havePreviousFix = false;
  bool haveStart = false;
  bool haveVario = false;
  double previousLat = 0.0;
  double previousLon = 0.0;
  uint32_t previousUtc = 0;

  while (file.available()) {
    const char c = static_cast<char>(file.read());
    if (c == '\r') {
      continue;
    }
    if (c != '\n' && lineLen + 1 < sizeof(line)) {
      line[lineLen++] = c;
      continue;
    }

    line[lineLen] = '\0';
    lineLen = 0;
    if (line[0] == 'B') {
      uint32_t utc = 0;
      uint32_t pressAlt = 0;
      uint32_t gpsAlt = 0;
      double lat = 0.0;
      double lon = 0.0;
      bool valid = false;
      if (parseBRecord(line, utc, lat, lon, valid, pressAlt, gpsAlt) && valid) {
        if (!haveStart) {
          stats.startUtcSeconds = utc;
          haveStart = true;
          stats.maxGpsAltM = static_cast<float>(gpsAlt);
          stats.minGpsAltM = static_cast<float>(gpsAlt);
          stats.maxPressAltM = static_cast<float>(pressAlt);
          stats.minPressAltM = static_cast<float>(pressAlt);
        }
        stats.endUtcSeconds = utc;
        if (gpsAlt > stats.maxGpsAltM) stats.maxGpsAltM = static_cast<float>(gpsAlt);
        if (gpsAlt < stats.minGpsAltM) stats.minGpsAltM = static_cast<float>(gpsAlt);
        if (pressAlt > stats.maxPressAltM) stats.maxPressAltM = static_cast<float>(pressAlt);
        if (pressAlt < stats.minPressAltM) stats.minPressAltM = static_cast<float>(pressAlt);

        if (havePreviousFix) {
          const float segmentM = distanceMeters(previousLat, previousLon, lat, lon);
          uint32_t dt = utc >= previousUtc ? utc - previousUtc : (86400UL - previousUtc + utc);
          if (dt == 0) dt = 1;
          if (segmentM > 0.2F && segmentM < 120.0F * static_cast<float>(dt)) {
            stats.distanceKm += segmentM / 1000.0F;
            const float speedKmh = (segmentM / static_cast<float>(dt)) * 3.6F;
            if (speedKmh > stats.maxGroundSpeedKmh) {
              stats.maxGroundSpeedKmh = speedKmh;
            }
          }
        }
        previousLat = lat;
        previousLon = lon;
        previousUtc = utc;
        havePreviousFix = true;
        if (stats.fixCount < UINT16_MAX) {
          ++stats.fixCount;
        }
      }
    } else if (line[0] == 'K') {
      float vario = 0.0F;
      if (parseKRecordVario(line, vario)) {
        if (!haveVario) {
          stats.maxVarioMs = vario;
          stats.minVarioMs = vario;
          haveVario = true;
        } else {
          if (vario > stats.maxVarioMs) stats.maxVarioMs = vario;
          if (vario < stats.minVarioMs) stats.minVarioMs = vario;
        }
      }
    }
  }
  file.close();

  if (haveStart && stats.fixCount > 0) {
    stats.durationSeconds = stats.endUtcSeconds >= stats.startUtcSeconds
                                ? stats.endUtcSeconds - stats.startUtcSeconds
                                : (86400UL - stats.startUtcSeconds + stats.endUtcSeconds);
    if (stats.durationSeconds > 0) {
      stats.averageGroundSpeedKmh = stats.distanceKm / (static_cast<float>(stats.durationSeconds) / 3600.0F);
    }
    stats.valid = true;
  }
  return stats.valid;
}

bool FlightRecorder::deleteTracklog(const char* filepath) {
  if (!storageReady_ || !fs_ || !filepath || filepath[0] == '\0') {
    return false;
  }
  fs::FS* sourceFs = nullptr;
  bool external = false;
  char normalizedPath[96];
  if (!resolveTracklogPath(filepath, sourceFs, normalizedPath, sizeof(normalizedPath), external)) {
    return false;
  }
  if (!external && logger_.isActive() && strcmp(normalizedPath, currentPath_) == 0) {
    return false;
  }
  return sourceFs->remove(normalizedPath);
}

bool FlightRecorder::moveTracklogsTo(fs::FS& destinationFs, const char* destinationDir, uint16_t& movedCount, uint16_t& failedCount) {
  movedCount = 0;
  failedCount = 0;
  if (!storageReady_ || !fs_ || !destinationDir || destinationDir[0] != '/' || recording()) {
    return false;
  }

  if (!destinationFs.exists("/brvario")) {
    destinationFs.mkdir("/brvario");
  }
  if (!destinationFs.exists(destinationDir)) {
    destinationFs.mkdir(destinationDir);
  }

  File root = fs_->open("/igc");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return false;
  }

  uint8_t buffer[512];
  File entry = root.openNextFile();
  while (entry) {
    if (!entry.isDirectory() && isIgcFilename(entry.name())) {
      char name[40];
      char sourcePath[64];
      char destinationPath[96];
      copyFileName(entry.name(), name, sizeof(name));
      snprintf(sourcePath, sizeof(sourcePath), "/igc/%s", name);
      snprintf(destinationPath, sizeof(destinationPath), "%s/%s", destinationDir, name);
      const uint32_t sourceSize = static_cast<uint32_t>(entry.size());
      entry.close();

      destinationFs.remove(destinationPath);
      File source = fs_->open(sourcePath, FILE_READ);
      File destination = destinationFs.open(destinationPath, FILE_WRITE);
      bool ok = source && destination;
      uint32_t writtenTotal = 0;
      while (ok && source.available()) {
        const size_t readBytes = source.read(buffer, sizeof(buffer));
        if (readBytes == 0) {
          break;
        }
        const size_t written = destination.write(buffer, readBytes);
        writtenTotal += static_cast<uint32_t>(written);
        if (written != readBytes) {
          ok = false;
        }
      }
      if (destination) {
        destination.flush();
        destination.close();
      }
      if (source) source.close();

      ok = ok && writtenTotal == sourceSize;
      if (ok) {
        fs_->remove(sourcePath);
        ++movedCount;
      } else {
        destinationFs.remove(destinationPath);
        ++failedCount;
      }
      entry = root.openNextFile();
      continue;
    }
    entry.close();
    entry = root.openNextFile();
  }
  root.close();
  return movedCount > 0 && failedCount == 0;
}

bool FlightRecorder::consumeCompletedFlight(char* filepath, size_t filepathSize) {
  if (!completedFlightPending_) {
    return false;
  }
  if (filepath && filepathSize > 0) {
    snprintf(filepath, filepathSize, "%s", completedPath_);
  }
  completedFlightPending_ = false;
  return completedPath_[0] != '\0';
}

bool FlightRecorder::consumeTakeoffDetected() {
  if (!takeoffDetectedPending_) {
    return false;
  }
  takeoffDetectedPending_ = false;
  return true;
}

uint32_t FlightRecorder::storageUsedBytes() const {
  if (!storageReady_ || !fs_) {
    return 0UL;
  }

  File root = fs_->open("/igc");
  if (!root || !root.isDirectory()) {
    return 0UL;
  }

  uint32_t used = 0;
  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory() && isIgcFilename(file.name())) {
      used += static_cast<uint32_t>(file.size());
    }
    file.close();
    file = root.openNextFile();
  }
  root.close();
  return used;
}

uint32_t FlightRecorder::storageTotalBytes() const {
  return kEstimatedStorageTotalBytes;
}

uint8_t FlightRecorder::storageUsedPercent() const {
  const uint32_t total = storageTotalBytes();
  if (total == 0) return 0;
  uint32_t percent = (storageUsedBytes() * 100UL) / total;
  if (percent > 100UL) percent = 100UL;
  return static_cast<uint8_t>(percent);
}

bool FlightRecorder::storageNearFull() const {
  const uint32_t total = storageTotalBytes();
  if (total == 0) return true;
  const uint32_t used = storageUsedBytes();
  const uint32_t freeBytes = total > used ? total - used : 0UL;
  return storageUsedPercent() >= 90 || freeBytes < 300UL * 1024UL;
}

void FlightRecorder::pushBackfill(const FlightSample& sample) {
  backfill_[backfillNext_] = sample;
  backfillNext_ = static_cast<uint8_t>((backfillNext_ + 1) % kBackfillSamples);
  if (backfillCount_ < kBackfillSamples) {
    ++backfillCount_;
  }
}

bool FlightRecorder::startFlight(const FlightSample& sample, const struct tm& utcDateTime) {
  if (!makeFilePath(utcDateTime)) {
    state_ = State::StorageError;
    return false;
  }
  if (!activeLogExternal_ && storageNearFull()) {
    state_ = State::StorageFull;
    Serial.println("Tracklog: memoria interna cheia, novo IGC nao sera criado.");
    return false;
  }

  logger_.setFlightDate(static_cast<uint8_t>(utcDateTime.tm_mday),
                        static_cast<uint8_t>(utcDateTime.tm_mon + 1),
                        static_cast<uint16_t>(utcDateTime.tm_year + 1900));
  if (!activeLogFs_ || !logger_.startFlight(*activeLogFs_, currentPath_, profile_->pilotName(), profile_->equipment(), profile_->gliderId())) {
    state_ = State::StorageError;
    return false;
  }

  flightStartMs_ = sample.monotonicMs;
  flightElapsedSeconds_ = 0;
  state_ = State::Recording;
  takeoffDetectedPending_ = true;
  resetLandingCandidate();
  noFixLandingCandidateMs_ = 0;
  logBackfill();
  Serial.print(activeLogExternal_ ? "Tracklog IGC iniciado no SD: " : "Tracklog IGC iniciado LOCAL: ");
  Serial.println(currentPath_);
  return true;
}

void FlightRecorder::logBackfill() {
  if (!logger_.isActive() || backfillCount_ == 0) {
    return;
  }

  const uint8_t start = backfillCount_ == kBackfillSamples ? backfillNext_ : 0;
  for (uint8_t i = 0; i < backfillCount_; ++i) {
    const uint8_t index = static_cast<uint8_t>((start + i) % kBackfillSamples);
    logSample(backfill_[index]);
  }
}

void FlightRecorder::logSample(const FlightSample& sample) {
  if (!logger_.isActive()) {
    state_ = State::StorageError;
    return;
  }
  if (!logger_.logFix(sample.utcSeconds, sample.latDeg, sample.lonDeg, sample.gpsFix, sample.pressAltM, sample.gpsAltM)) {
    state_ = State::StorageError;
    return;
  }
  if (!logger_.logExtensionK(sample.utcSeconds, sample.varioMs)) {
    state_ = State::StorageError;
  }
}

bool FlightRecorder::makeFilePath(const struct tm& utcDateTime) {
  if (!fs_) {
    return false;
  }

  activeLogFs_ = fs_;
  activeLogExternal_ = false;
  const char* directory = "/igc";
  if (archiveReady_ && archiveFs_ && archiveDir_[0] != '\0') {
    bool archiveOk = true;
    if (!archiveFs_->exists("/brvario")) {
      archiveOk = archiveFs_->mkdir("/brvario");
    }
    if (archiveOk && !archiveFs_->exists(archiveDir_)) {
      archiveOk = archiveFs_->mkdir(archiveDir_);
    }
    if (archiveOk) {
      activeLogFs_ = archiveFs_;
      activeLogExternal_ = true;
      directory = archiveDir_;
    }
  }

  if (!activeLogFs_->exists(directory)) {
    activeLogFs_->mkdir(directory);
  }

  const int year = utcDateTime.tm_year + 1900;
  const int month = utcDateTime.tm_mon + 1;
  const int day = utcDateTime.tm_mday;
  for (uint8_t index = 1; index <= 99; ++index) {
    snprintf(currentPath_,
             sizeof(currentPath_),
             "%s/%04d-%02d-%02d-%02d%02d%02d-%02u.igc",
             directory,
             year,
             month,
             day,
             utcDateTime.tm_hour,
             utcDateTime.tm_min,
             utcDateTime.tm_sec,
             static_cast<unsigned>(index));
    if (!activeLogFs_->exists(currentPath_)) {
      return true;
    }
  }
  currentPath_[0] = '\0';
  return false;
}

bool FlightRecorder::detectTakeoff(const FlightSample& sample) {
  const bool hdopKnown = sample.gpsHdop > 0.0F && sample.gpsHdop < 90.0F;
  const bool gpsQualityOk = sample.satellites >= kTakeoffMinSatellites && (!hdopKnown || sample.gpsHdop <= kTakeoffMaxHdop);
  if (!sample.gpsFix || !gpsQualityOk) {
    takeoffCandidateMs_ = 0;
    speedFilterValid_ = false;
    return false;
  }

  if (takeoffCandidateMs_ == 0) {
    takeoffCandidateMs_ = sample.monotonicMs;
    takeoffLatDeg_ = sample.latDeg;
    takeoffLonDeg_ = sample.lonDeg;
    return false;
  }

  const float moveM = distanceMeters(takeoffLatDeg_, takeoffLonDeg_, sample.latDeg, sample.lonDeg);
  const uint32_t candidateAgeMs = sample.monotonicMs - takeoffCandidateMs_;
  const bool sustainedSpeed = filteredGroundSpeedKmh_ >= kTakeoffSpeedKmh && sample.groundSpeedKmh >= kTakeoffFallbackSpeedKmh;
  const bool normalTakeoff = candidateAgeMs >= kTakeoffConfirmMs && moveM >= kTakeoffMinMoveM && sustainedSpeed;
  const bool displacementTakeoff = candidateAgeMs >= kTakeoffConfirmMs && moveM >= kTakeoffFallbackMoveM &&
                                   filteredGroundSpeedKmh_ >= kTakeoffFallbackSpeedKmh && sample.groundSpeedKmh >= kTakeoffFallbackSpeedKmh;
  if (normalTakeoff || displacementTakeoff) {
    Serial.printf("Tracklog: decolagem detectada move=%.1fm speed=%.1fkm/h filt=%.1fkm/h sats=%u hdop=%.1f\n",
                  static_cast<double>(moveM),
                  static_cast<double>(sample.groundSpeedKmh),
                  static_cast<double>(filteredGroundSpeedKmh_),
                  static_cast<unsigned>(sample.satellites),
                  static_cast<double>(sample.gpsHdop));
    return true;
  }

  if (candidateAgeMs > kTakeoffReferenceResetMs && moveM < 5.0F && sample.groundSpeedKmh < kTakeoffFallbackSpeedKmh) {
    takeoffCandidateMs_ = sample.monotonicMs;
    takeoffLatDeg_ = sample.latDeg;
    takeoffLonDeg_ = sample.lonDeg;
  }
  return false;
}

bool FlightRecorder::detectLanding(const FlightSample& sample) {
  if (!sample.gpsFix || flightStartMs_ == 0 || sample.monotonicMs - flightStartMs_ < kLandingMinFlightMs) {
    resetLandingCandidate();
    return false;
  }

  const bool lowSpeed = filteredGroundSpeedKmh_ <= kLandingSpeedKmh || sample.groundSpeedKmh <= kLandingSpeedKmh;
  const bool movingAgain = filteredGroundSpeedKmh_ >= kLandingResetSpeedKmh && sample.groundSpeedKmh >= kLandingResetSpeedKmh;
  if (movingAgain) {
    resetLandingCandidate();
    return false;
  }

  const float absVario = fabsf(sample.varioMs);
  if (!landingVarioFilterValid_) {
    landingFilteredAbsVarioMs_ = absVario;
    landingVarioFilterValid_ = true;
  } else {
    landingFilteredAbsVarioMs_ = landingFilteredAbsVarioMs_ * 0.94F + absVario * 0.06F;
  }

  if (landingCandidateMs_ == 0) {
    landingCandidateMs_ = sample.monotonicMs;
    landingLatDeg_ = sample.latDeg;
    landingLonDeg_ = sample.lonDeg;
    landingMinAltM_ = sample.pressAltM;
    landingMaxAltM_ = sample.pressAltM;
    landingLowSpeedAccumMs_ = 0;
    landingLastCheckMs_ = sample.monotonicMs;
    Serial.println("Tracklog: candidato de pouso iniciado.");
    return false;
  }

  uint32_t dtMs = sample.monotonicMs >= landingLastCheckMs_ ? sample.monotonicMs - landingLastCheckMs_ : 0;
  if (dtMs > 1000UL) {
    dtMs = 1000UL;
  }
  landingLastCheckMs_ = sample.monotonicMs;
  if (lowSpeed) {
    landingLowSpeedAccumMs_ += dtMs;
  } else if (landingLowSpeedAccumMs_ > dtMs) {
    landingLowSpeedAccumMs_ -= dtMs;
  } else {
    landingLowSpeedAccumMs_ = 0;
  }

  if (sample.pressAltM < landingMinAltM_) landingMinAltM_ = sample.pressAltM;
  if (sample.pressAltM > landingMaxAltM_) landingMaxAltM_ = sample.pressAltM;

  const float moveM = distanceMeters(landingLatDeg_, landingLonDeg_, sample.latDeg, sample.lonDeg);
  if (moveM > kLandingFallbackMaxMoveM) {
    landingCandidateMs_ = sample.monotonicMs;
    landingLatDeg_ = sample.latDeg;
    landingLonDeg_ = sample.lonDeg;
    landingMinAltM_ = sample.pressAltM;
    landingMaxAltM_ = sample.pressAltM;
    landingLowSpeedAccumMs_ = lowSpeed ? dtMs : 0;
    landingLastCheckMs_ = sample.monotonicMs;
    Serial.println("Tracklog: candidato de pouso reiniciado por deslocamento GPS.");
    return false;
  }

  const float altitudeChangeM = landingMaxAltM_ - landingMinAltM_;
  const bool nearGround = sample.aglM >= 0.0F && sample.aglM <= kLandingNearGroundAglM;
  const bool varioCalm = landingFilteredAbsVarioMs_ <= kLandingCalmVarioMs || absVario <= kLandingMomentaryCalmVarioMs;
  const bool gpsStationary = moveM <= kLandingMaxMoveM;
  const bool altitudeStable = altitudeChangeM <= kLandingMaxAltitudeChangeM;
  const bool stoppedOnGround = landingLowSpeedAccumMs_ >= kLandingConfirmMs && gpsStationary && filteredGroundSpeedKmh_ <= 2.0F &&
                               sample.groundSpeedKmh <= 2.0F;
  const bool xctrackStyleLanding = landingLowSpeedAccumMs_ >= kLandingConfirmMs && (gpsStationary || altitudeStable || nearGround);
  const bool conservativeLanding = landingLowSpeedAccumMs_ >= kLandingFallbackConfirmMs && (varioCalm || altitudeStable) &&
                                   moveM <= kLandingFallbackMaxMoveM;

  if (stoppedOnGround || xctrackStyleLanding || conservativeLanding) {
    Serial.printf("Tracklog: pouso detectado lowSpeed=%lus move=%.1fm altSpan=%.1fm vario=%.2f varioAvg=%.2f agl=%.1f\n",
                  static_cast<unsigned long>(landingLowSpeedAccumMs_ / 1000UL),
                  static_cast<double>(moveM),
                  static_cast<double>(altitudeChangeM),
                  static_cast<double>(sample.varioMs),
                  static_cast<double>(landingFilteredAbsVarioMs_),
                  static_cast<double>(sample.aglM));
    return true;
  }

  return false;
}

void FlightRecorder::resetLandingCandidate() {
  landingCandidateMs_ = 0;
  landingLowSpeedAccumMs_ = 0;
  landingLastCheckMs_ = 0;
  landingLatDeg_ = 0.0;
  landingLonDeg_ = 0.0;
  landingMinAltM_ = 0.0F;
  landingMaxAltM_ = 0.0F;
  landingFilteredAbsVarioMs_ = 0.0F;
  landingVarioFilterValid_ = false;
}

void FlightRecorder::updateElapsed(VarioData& data, uint32_t now) {
  if (state_ == State::Recording && flightStartMs_ != 0) {
    flightElapsedSeconds_ = (now - flightStartMs_) / 1000UL;
  }
  data.trackingEnabled = state_ == State::Recording;
  data.elapsedSeconds = flightElapsedSeconds_;
}

void FlightRecorder::updateSpeedFilter(float groundSpeedKmh) {
  if (!speedFilterValid_) {
    filteredGroundSpeedKmh_ = groundSpeedKmh;
    speedFilterValid_ = true;
    return;
  }
  filteredGroundSpeedKmh_ = filteredGroundSpeedKmh_ * 0.72F + groundSpeedKmh * 0.28F;
}

FlightRecorder::FlightSample FlightRecorder::makeSample(const VarioData& data, const struct tm& utcDateTime, uint32_t now) {
  FlightSample sample;
  sample.monotonicMs = now;
  sample.utcSeconds = utcSecondsFromTm(utcDateTime);
  sample.latDeg = data.latitudeDeg;
  sample.lonDeg = data.longitudeDeg;
  sample.pressAltM = data.altitudeM;
  sample.gpsAltM = data.altitudeGpsM;
  sample.aglM = data.altitudeAglM;
  sample.groundSpeedKmh = data.groundSpeedKmh;
  sample.varioMs = data.varioMs;
  sample.gpsHdop = data.gpsHdop;
  sample.satellites = data.satellites;
  sample.gpsFix = data.gpsFix;
  return sample;
}

uint32_t FlightRecorder::utcSecondsFromTm(const struct tm& utcDateTime) {
  return static_cast<uint32_t>(utcDateTime.tm_hour) * 3600UL + static_cast<uint32_t>(utcDateTime.tm_min) * 60UL +
         static_cast<uint32_t>(utcDateTime.tm_sec);
}

float FlightRecorder::distanceMeters(double lat1, double lon1, double lat2, double lon2) {
  const float midLat = degToRad((lat1 + lat2) * 0.5);
  const float dLat = degToRad(lat2 - lat1);
  const float dLon = degToRad(lon2 - lon1);
  const float x = dLon * cosf(midLat) * kEarthRadiusM;
  const float y = dLat * kEarthRadiusM;
  return sqrtf(x * x + y * y);
}

bool FlightRecorder::parseBRecord(const char* line,
                                  uint32_t& utcSeconds,
                                  double& latDeg,
                                  double& lonDeg,
                                  bool& valid,
                                  uint32_t& pressAltM,
                                  uint32_t& gpsAltM) {
  if (!line || strlen(line) < 35 || line[0] != 'B') {
    return false;
  }
  utcSeconds = parseUtc(&line[1]);
  valid = line[24] == 'A';
  pressAltM = parseUnsignedFixed(&line[25], 5);
  gpsAltM = parseUnsignedFixed(&line[30], 5);
  return parseIgcCoordinate(&line[7], true, latDeg) && parseIgcCoordinate(&line[15], false, lonDeg);
}

bool FlightRecorder::parseKRecordVario(const char* line, float& varioMs) {
  if (!line || strlen(line) < 11 || line[0] != 'K') {
    return false;
  }
  const char sign = line[7];
  if (sign != '+' && sign != '-') {
    return false;
  }
  const uint32_t tenths = parseUnsignedFixed(&line[8], 3);
  varioMs = static_cast<float>(tenths) / 10.0F;
  if (sign == '-') {
    varioMs = -varioMs;
  }
  return true;
}

uint32_t FlightRecorder::parseUtc(const char* text) {
  if (!text) return 0;
  const uint32_t hour = parseUnsignedFixed(text, 2);
  const uint32_t minute = parseUnsignedFixed(text + 2, 2);
  const uint32_t second = parseUnsignedFixed(text + 4, 2);
  return (hour % 24UL) * 3600UL + (minute % 60UL) * 60UL + (second % 60UL);
}

uint32_t FlightRecorder::parseUnsignedFixed(const char* text, uint8_t len) {
  uint32_t value = 0;
  if (!text) return value;
  for (uint8_t i = 0; i < len; ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') {
      return value;
    }
    value = value * 10UL + static_cast<uint32_t>(c - '0');
  }
  return value;
}

bool FlightRecorder::parseIgcCoordinate(const char* text, bool latitude, double& degrees) {
  if (!text) return false;
  const uint8_t degLen = latitude ? 2 : 3;
  const uint8_t hemiIndex = latitude ? 7 : 8;
  const char hemi = text[hemiIndex];
  if (latitude) {
    if (hemi != 'N' && hemi != 'S') return false;
  } else if (hemi != 'E' && hemi != 'W') {
    return false;
  }

  const uint32_t wholeDeg = parseUnsignedFixed(text, degLen);
  const uint32_t minutes = parseUnsignedFixed(text + degLen, 2);
  const uint32_t milliMinutes = parseUnsignedFixed(text + degLen + 2, 3);
  degrees = static_cast<double>(wholeDeg) + (static_cast<double>(minutes) + static_cast<double>(milliMinutes) / 1000.0) / 60.0;
  if (hemi == 'S' || hemi == 'W') {
    degrees = -degrees;
  }
  return true;
}

void FlightRecorder::formatEntryDateTime(const char* name, char* dateOut, size_t dateSize, char* timeOut, size_t timeSize) {
  if (dateOut && dateSize > 0) dateOut[0] = '\0';
  if (timeOut && timeSize > 0) timeOut[0] = '\0';
  if (!name || strlen(name) < 10) {
    return;
  }

  const bool dateLooksValid = name[0] >= '0' && name[0] <= '9' && name[1] >= '0' && name[1] <= '9' && name[2] >= '0' &&
                              name[2] <= '9' && name[3] >= '0' && name[3] <= '9' && name[4] == '-' && name[7] == '-';
  if (!dateLooksValid) {
    return;
  }

  int year = static_cast<int>(parseFourDigits(&name[0]));
  int month = static_cast<int>(parseTwoDigits(&name[5]));
  int day = static_cast<int>(parseTwoDigits(&name[8]));
  int hour = 0;
  int minute = 0;
  if (strlen(name) >= 18 && name[10] == '-' && name[11] >= '0' && name[11] <= '9' && name[16] >= '0' && name[16] <= '9') {
    hour = static_cast<int>(parseTwoDigits(&name[11]));
    minute = static_cast<int>(parseTwoDigits(&name[13]));
    int totalSeconds = hour * 3600 + minute * 60 + kDisplayUtcOffsetSeconds;
    while (totalSeconds < 0) {
      totalSeconds += 86400;
      adjustDateParts(year, month, day, -1);
    }
    while (totalSeconds >= 86400) {
      totalSeconds -= 86400;
      adjustDateParts(year, month, day, 1);
    }
    hour = totalSeconds / 3600;
    minute = (totalSeconds % 3600) / 60;
  }

  if (dateOut && dateSize > 0) {
    snprintf(dateOut, dateSize, "%02d/%02d/%02d", day, month, year % 100);
  }

  if (timeOut && timeSize > 0) {
    snprintf(timeOut, timeSize, "%02d:%02d", hour, minute);
  }
}

bool FlightRecorder::isIgcFilename(const char* name) {
  if (!name) {
    return false;
  }
  const size_t len = strlen(name);
  if (len < 4) {
    return false;
  }
  const char* ext = name + len - 4;
  return (ext[0] == '.') && (ext[1] == 'i' || ext[1] == 'I') && (ext[2] == 'g' || ext[2] == 'G') &&
         (ext[3] == 'c' || ext[3] == 'C');
}

void FlightRecorder::copyFileName(const char* path, char* out, size_t outSize) {
  if (!out || outSize == 0) {
    return;
  }
  const char* name = path ? path : "";
  for (const char* cursor = name; *cursor != '\0'; ++cursor) {
    if (*cursor == '/' || *cursor == '\\') {
      name = cursor + 1;
    }
  }
  snprintf(out, outSize, "%s", name);
}

uint16_t FlightRecorder::countTracklogsIn(fs::FS* fs, const char* directory) {
  if (!fs || !directory || directory[0] == '\0') {
    return 0;
  }

  File root = fs->open(directory);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return 0;
  }

  uint16_t count = 0;
  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory() && isIgcFilename(file.name()) && count < UINT16_MAX) {
      ++count;
    }
    file.close();
    file = root.openNextFile();
  }
  root.close();
  return count;
}

bool FlightRecorder::resolveTracklogPath(const char* filepath,
                                         fs::FS*& sourceFs,
                                         char* normalizedPath,
                                         size_t pathSize,
                                         bool& external) const {
  sourceFs = nullptr;
  external = false;
  if (normalizedPath && pathSize > 0) {
    normalizedPath[0] = '\0';
  }
  if (!filepath || filepath[0] == '\0' || !normalizedPath || pathSize == 0) {
    return false;
  }

  if (strncmp(filepath, kSdTracklogPrefix, strlen(kSdTracklogPrefix)) == 0) {
    if (!archiveReady_ || !archiveFs_) {
      return false;
    }
    const char* sdPath = filepath + strlen(kSdTracklogPrefix);
    if (sdPath[0] != '/') {
      return false;
    }
    snprintf(normalizedPath, pathSize, "%s", sdPath);
    sourceFs = archiveFs_;
    external = true;
    return true;
  }

  if (strncmp(filepath, "/brvario/igc/", 13) == 0) {
    if (!archiveReady_ || !archiveFs_) {
      return false;
    }
    snprintf(normalizedPath, pathSize, "%s", filepath);
    sourceFs = archiveFs_;
    external = true;
    return true;
  }

  if (!fs_) {
    return false;
  }
  normalizeIgcPath(filepath, normalizedPath, pathSize);
  sourceFs = fs_;
  external = false;
  return normalizedPath[0] != '\0';
}
