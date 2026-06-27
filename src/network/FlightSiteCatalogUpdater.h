#pragma once

#include <Arduino.h>
#include <FS.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <stdint.h>

class WifiManager;

enum class FlightSiteCatalogUpdateState : uint8_t {
  Idle = 0,
  WaitingWifi,
  Downloading,
  Validating,
  Success,
  Failed,
  Canceled,
};

class FlightSiteCatalogUpdater {
 public:
  void begin(fs::FS* filesystem, WifiManager* wifi);
  bool requestUpdate();
  void update();
  void cancel();
  void resetStatus();

  bool busy() const {
    return state_ == FlightSiteCatalogUpdateState::WaitingWifi ||
           state_ == FlightSiteCatalogUpdateState::Downloading ||
           state_ == FlightSiteCatalogUpdateState::Validating;
  }
  bool isConfigured() const;
  FlightSiteCatalogUpdateState state() const { return state_; }
  const char* statusText() const { return statusText_; }
  uint8_t progressPercent() const { return progressPercent_; }
  uint32_t downloadedBytes() const { return downloadedBytes_; }

 private:
  static constexpr char kTempPath[] = "/weather/catalog.tmp";
  static constexpr char kBackupPath[] = "/weather/catalog.bak";

  fs::FS* filesystem_ = nullptr;
  WifiManager* wifi_ = nullptr;
  FlightSiteCatalogUpdateState state_ = FlightSiteCatalogUpdateState::Idle;
  uint8_t progressPercent_ = 0;
  uint32_t downloadedBytes_ = 0;
  int32_t contentLength_ = -1;
  uint32_t waitStartedMs_ = 0;
  uint32_t lastDataMs_ = 0;
  char statusText_[72] = "CATALOGO PRONTO";
  uint8_t chunk_[1024] = {};
  File output_;
  HTTPClient http_;
  WiFiClient plainClient_;
  WiFiClientSecure secureClient_;

  bool startDownload();
  void finishDownload();
  void fail(const char* text);
  void setStatus(const char* text);
  void closeTransfer();
  bool ensureDirectory();
};
