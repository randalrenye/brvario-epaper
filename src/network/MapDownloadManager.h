#pragma once

#include <Arduino.h>
#include <FS.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <stdint.h>

class StorageManager;
class WifiManager;

enum class MapDownloadState : uint8_t {
  Idle = 0,
  WaitingWifi,
  NoWifi,
  NoSd,
  NoUrl,
  Downloading,
  Success,
  Failed,
  Canceled,
};

enum class MapDownloadView : uint8_t {
  MacroRegions = 0,
  States,
  Packages,
};

enum class MapDownloadTargetKind : uint8_t {
  CurrentRegion = 0,
  MacroRegion,
  State,
  StateBundle,
  Package,
  PagePrev,
  PageNext,
  DownloadedMaps,
  DeleteMaps,
};

struct MapRegionInfo {
  const char* id;
  const char* name;
  const char* fileName;
  float centerLat;
  float centerLon;
  uint16_t approxSizeMb;
};

struct MapDownloadTargetInfo {
  const char* id;
  const char* name;
  const char* subtitle;
  const char* fileName;
  float centerLat;
  float centerLon;
  uint16_t radiusKm;
  uint16_t approxSizeMb;
  MapDownloadTargetKind kind;
  uint8_t macroIndex;
  uint8_t stateIndex;
  uint16_t packageIndex;
};

class MapDownloadManager {
 public:
  void begin(StorageManager* storage, WifiManager* wifi);
  void update();

  uint8_t targetCount() const;
  const MapDownloadTargetInfo& target(uint8_t index) const;
  uint8_t selectedTarget() const { return selectedTarget_; }
  void selectTarget(uint8_t index);
  MapDownloadView view() const { return view_; }
  const char* titleText() const;
  const char* subtitleText() const;
  bool canGoBack() const { return view_ != MapDownloadView::MacroRegions; }
  void goBack();
  bool targetDownloaded(uint8_t index) const;
  bool beginDownloadSelected(float currentLat, float currentLon, bool hasFix);
  bool beginDownload(uint8_t index, float currentLat, float currentLon, bool hasFix);

  uint8_t regionCount() const;
  const MapRegionInfo& region(uint8_t index) const;
  uint8_t selectedRegion() const { return selectedTarget_; }
  void selectRegion(uint8_t index);

  bool beginDownloadSelected();
  bool beginDownload(uint8_t index);
  bool executeSelected(float currentLat, float currentLon, bool hasFix) { return beginDownloadSelected(currentLat, currentLon, hasFix); }
  void cancel();
  void resetStatus();

  bool busy() const { return state_ == MapDownloadState::WaitingWifi || state_ == MapDownloadState::Downloading; }
  bool isConfigured() const;
  bool regionDownloaded(uint8_t index) const;
  MapDownloadState state() const { return state_; }
  const char* statusText() const { return statusText_; }
  const char* errorText() const { return errorText_; }
  uint8_t progressPercent() const { return progressPercent_; }
  uint32_t downloadedBytes() const { return downloadedBytes_; }
  int32_t contentLength() const { return contentLength_; }

 private:
  StorageManager* storage_ = nullptr;
  WifiManager* wifi_ = nullptr;
  MapDownloadState state_ = MapDownloadState::Idle;
  MapDownloadView view_ = MapDownloadView::MacroRegions;
  uint8_t selectedTarget_ = 0;
  uint8_t selectedMacroIndex_ = 0;
  uint8_t selectedStateIndex_ = 0;
  uint16_t browserPage_ = 0;
  uint8_t optionCount_ = 0;
  static constexpr uint8_t kMaxOptions = 6;
  static constexpr uint16_t kNoPackageIndex = 0xFFFF;
  static constexpr uint16_t kMaxBulkQueue = 1400;
  MapDownloadTargetInfo options_[kMaxOptions] = {};
  char optionIds_[kMaxOptions][20] = {};
  char optionNames_[kMaxOptions][28] = {};
  char optionSubtitles_[kMaxOptions][28] = {};
  char optionFiles_[kMaxOptions][32] = {};
  MapDownloadTargetInfo activeTargetInfo_ = {};
  char activeId_[20] = {};
  char activeName_[28] = {};
  char activeFile_[32] = {};
  uint16_t activePackageIndex_ = kNoPackageIndex;
  bool bulkActive_ = false;
  uint16_t bulkQueue_[kMaxBulkQueue] = {};
  uint16_t bulkCount_ = 0;
  uint16_t bulkIndex_ = 0;
  uint16_t bulkDownloaded_ = 0;
  uint8_t progressPercent_ = 0;
  uint32_t downloadedBytes_ = 0;
  int32_t contentLength_ = -1;
  uint32_t waitStartedMs_ = 0;
  uint32_t lastDataMs_ = 0;
  float activeCenterLat_ = 0.0F;
  float activeCenterLon_ = 0.0F;
  uint16_t activeRadiusKm_ = 0;
  bool deleteConfirm_ = false;
  char statusText_[64] = "PRONTO";
  char errorText_[80] = "";
  char url_[192] = "";
  char tempPath_[96] = "";
  char finalPath_[96] = "";
  uint8_t chunk_[1024] = {};
  File output_;
  HTTPClient http_;
  WiFiClient plainClient_;
  WiFiClientSecure secureClient_;

  void startWhenWifiReady();
  bool startHttpDownload();
  void finishDownload();
  void fail(const char* message);
  void setStatus(const char* text);
  void buildPaths(const MapDownloadTargetInfo& info);
  void buildUrl(const MapDownloadTargetInfo& info);
  void closeTransfer();
  bool createRegionManifest();
  bool executeLocalAction(uint8_t index, float currentLat, float currentLon, bool hasFix);
  uint16_t packageForCoordinate(float lat, float lon) const;
  bool targetFileExists(const char* fileName) const;
  bool targetFileExistsForPackageIndex(uint16_t packageIndex) const;
  bool manifestExists(const char* id) const;
  void refreshOptions();
  void setOption(uint8_t slot,
                 MapDownloadTargetKind kind,
                 const char* id,
                 const char* name,
                 const char* subtitle,
                 const char* fileName,
                 float centerLat,
                 float centerLon,
                 uint16_t radiusKm,
                 uint16_t approxSizeMb,
                 uint8_t macroIndex,
                 uint8_t stateIndex,
                 uint16_t packageIndex);
  void setPackageOption(uint8_t slot, uint16_t packageIndex);
  uint8_t stateCountForMacro(uint8_t macroIndex) const;
  uint8_t stateIndexForMacroOffset(uint8_t macroIndex, uint8_t offset) const;
  void enterMacro(uint8_t macroIndex);
  void enterState(uint8_t stateIndex);
  bool beginPackageDownload(const MapDownloadTargetInfo& info);
  bool beginStateDownload(uint8_t stateIndex);
  bool startNextQueuedDownload();
  void buildStateQueue(uint8_t stateIndex);
  bool queueContains(uint16_t packageIndex) const;
  bool packageTouchesStatePackage(uint16_t packageIndex, uint8_t stateIndex) const;
};
