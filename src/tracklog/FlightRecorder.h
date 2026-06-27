#pragma once

#include <Arduino.h>
#include <FS.h>
#include <time.h>
#include <stdint.h>

#include "config/PilotProfileConfig.h"
#include "data/VarioData.h"
#include "tracklog/IgcLogger.h"

class FlightRecorder {
 public:
  FlightRecorder();

  struct TracklogEntry {
    char path[64];
    char name[40];
    char displayDate[12];
    char displayTime[8];
    char location[8];
    uint32_t sizeBytes;
    bool external;
  };

  struct TracklogStats {
    char path[64];
    char name[40];
    char location[8];
    uint32_t sizeBytes = 0;
    uint32_t durationSeconds = 0;
    uint32_t startUtcSeconds = 0;
    uint32_t endUtcSeconds = 0;
    float maxGpsAltM = 0.0F;
    float minGpsAltM = 0.0F;
    float maxPressAltM = 0.0F;
    float minPressAltM = 0.0F;
    float maxVarioMs = 0.0F;
    float minVarioMs = 0.0F;
    float maxGroundSpeedKmh = 0.0F;
    float averageGroundSpeedKmh = 0.0F;
    float distanceKm = 0.0F;
    uint16_t fixCount = 0;
    bool valid = false;
  };

  bool begin(fs::FS& fs, PilotProfileConfig& profile);
  void attachArchiveStorage(fs::FS* fs, const char* directory);
  void update(VarioData& data, const struct tm& utcDateTime, bool hasUtcDateTime);
  void endFlight(VarioData& data);

  bool storageReady() const { return fs_ != nullptr && storageReady_; }
  bool recording() const { return state_ == State::Recording; }
  const char* statusText() const;
  const char* currentFilePath() const { return currentPath_; }
  uint32_t flightElapsedSeconds() const { return flightElapsedSeconds_; }
  uint16_t tracklogCount() const;
  bool tracklogPage(uint16_t page,
                    uint8_t pageSize,
                    TracklogEntry* entries,
                    uint8_t& entryCount,
                    uint16_t& totalCount,
                    uint32_t& usedBytes) const;
  bool tracklogEntry(uint16_t newestFirstIndex, TracklogEntry& entry) const;
  bool readTracklogChunk(const char* filepath,
                         uint32_t offset,
                         uint8_t* buffer,
                         size_t bufferSize,
                         size_t& bytesRead,
                         uint32_t& fileSize) const;
  bool tracklogStats(const char* filepath, TracklogStats& stats) const;
  bool deleteTracklog(const char* filepath);
  bool moveTracklogsTo(fs::FS& destinationFs, const char* destinationDir, uint16_t& movedCount, uint16_t& failedCount);
  bool consumeCompletedFlight(char* filepath, size_t filepathSize);
  bool consumeTakeoffDetected();
  uint32_t storageUsedBytes() const;
  uint32_t storageTotalBytes() const;
  uint8_t storageUsedPercent() const;
  bool storageNearFull() const;

 private:
  enum class State : uint8_t {
    WaitingGps,
    WaitingTakeoff,
    Recording,
    Landed,
    StorageError,
    StorageFull,
  };

  struct FlightSample {
    uint32_t monotonicMs;
    uint32_t utcSeconds;
    double latDeg;
    double lonDeg;
    float pressAltM;
    float gpsAltM;
    float aglM;
    float groundSpeedKmh;
    float varioMs;
    float gpsHdop;
    uint8_t satellites;
    bool gpsFix;
  };

  static constexpr uint8_t kBackfillSamples = 20;
  static constexpr float kTakeoffSpeedKmh = 6.0F;
  static constexpr float kTakeoffFallbackSpeedKmh = 3.5F;
  static constexpr uint32_t kTakeoffConfirmMs = 5000UL;
  static constexpr float kTakeoffMinMoveM = 18.0F;
  static constexpr float kTakeoffFallbackMoveM = 35.0F;
  static constexpr uint32_t kTakeoffReferenceResetMs = 120000UL;
  static constexpr uint8_t kTakeoffMinSatellites = 6;
  static constexpr float kTakeoffMaxHdop = 3.0F;
  static constexpr uint32_t kMinimumRecordedFlightMs = 60000UL;
  static constexpr float kLandingSpeedKmh = 6.0F;
  static constexpr float kLandingResetSpeedKmh = 10.0F;
  static constexpr float kLandingNearGroundAglM = 12.0F;
  static constexpr float kLandingMaxMoveM = 80.0F;
  static constexpr float kLandingFallbackMaxMoveM = 180.0F;
  static constexpr float kLandingMaxAltitudeChangeM = 8.0F;
  static constexpr float kLandingCalmVarioMs = 0.65F;
  static constexpr float kLandingMomentaryCalmVarioMs = 0.90F;
  static constexpr uint32_t kLandingConfirmMs = 25000UL;
  static constexpr uint32_t kLandingFallbackConfirmMs = 60000UL;
  static constexpr uint32_t kLandingMinFlightMs = 20000UL;
  static constexpr uint32_t kNoFixLandingConfirmMs = 45000UL;
  static constexpr uint32_t kEstimatedStorageTotalBytes = 1024UL * 1024UL;
  static constexpr uint8_t kTracklogListMaxEntries = 32;

  fs::FS* fs_ = nullptr;
  fs::FS* archiveFs_ = nullptr;
  fs::FS* activeLogFs_ = nullptr;
  PilotProfileConfig* profile_ = nullptr;
  IgcLogger logger_;
  State state_ = State::WaitingGps;
  FlightSample backfill_[kBackfillSamples] = {};
  uint8_t backfillNext_ = 0;
  uint8_t backfillCount_ = 0;
  uint32_t lastSampleUtcSecond_ = UINT32_MAX;
  uint32_t takeoffCandidateMs_ = 0;
  double takeoffLatDeg_ = 0.0;
  double takeoffLonDeg_ = 0.0;
  uint32_t flightStartMs_ = 0;
  uint32_t flightElapsedSeconds_ = 0;
  uint32_t landingCandidateMs_ = 0;
  uint32_t landingLowSpeedAccumMs_ = 0;
  uint32_t landingLastCheckMs_ = 0;
  uint32_t noFixLandingCandidateMs_ = 0;
  double landingLatDeg_ = 0.0;
  double landingLonDeg_ = 0.0;
  float landingMinAltM_ = 0.0F;
  float landingMaxAltM_ = 0.0F;
  float landingFilteredAbsVarioMs_ = 0.0F;
  float filteredGroundSpeedKmh_ = 0.0F;
  char currentPath_[64] = {};
  char completedPath_[64] = {};
  bool storageReady_ = false;
  bool speedFilterValid_ = false;
  bool landingVarioFilterValid_ = false;
  bool completedFlightPending_ = false;
  bool takeoffDetectedPending_ = false;
  bool archiveReady_ = false;
  bool activeLogExternal_ = false;
  char archiveDir_[32] = {};
  mutable TracklogEntry tracklogListScratch_[kTracklogListMaxEntries] = {};

  void pushBackfill(const FlightSample& sample);
  bool startFlight(const FlightSample& sample, const struct tm& utcDateTime);
  void logBackfill();
  void logSample(const FlightSample& sample);
  bool makeFilePath(const struct tm& utcDateTime);
  bool detectTakeoff(const FlightSample& sample);
  bool detectLanding(const FlightSample& sample);
  void resetLandingCandidate();
  void updateElapsed(VarioData& data, uint32_t now);
  void updateSpeedFilter(float groundSpeedKmh);
  static FlightSample makeSample(const VarioData& data, const struct tm& utcDateTime, uint32_t now);
  static uint32_t utcSecondsFromTm(const struct tm& utcDateTime);
  static float distanceMeters(double lat1, double lon1, double lat2, double lon2);
  static bool parseBRecord(const char* line, uint32_t& utcSeconds, double& latDeg, double& lonDeg, bool& valid, uint32_t& pressAltM,
                           uint32_t& gpsAltM);
  static bool parseKRecordVario(const char* line, float& varioMs);
  static uint32_t parseUtc(const char* text);
  static uint32_t parseUnsignedFixed(const char* text, uint8_t len);
  static bool parseIgcCoordinate(const char* text, bool latitude, double& degrees);
  static void formatEntryDateTime(const char* name, char* dateOut, size_t dateSize, char* timeOut, size_t timeSize);
  static bool isIgcFilename(const char* name);
  static void copyFileName(const char* path, char* out, size_t outSize);
  static uint16_t countTracklogsIn(fs::FS* fs, const char* directory);
  bool resolveTracklogPath(const char* filepath, fs::FS*& sourceFs, char* normalizedPath, size_t pathSize, bool& external) const;
};
