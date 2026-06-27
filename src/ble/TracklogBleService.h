#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

class BLECharacteristic;
class BLEServer;
class FlightRecorder;
class TracklogBleCommandCallbacks;
class TracklogBleServerCallbacks;

class TracklogBleService {
 public:
  TracklogBleService();

  bool begin(FlightRecorder* recorder);
  void update();
  void end();

  bool connected() const { return connected_; }
  bool activeTransfer() const { return transferActive_; }
  bool enabled() const { return ready_ && bleWindowActive_; }
  bool visibleStatus() const;
  const char* statusText() const { return statusText_; }
  const char* currentFileName() const { return transferName_; }
  uint8_t progressPercent() const;
  uint16_t syncTotal() const { return syncTotal_; }
  uint16_t syncSentCount() const { return syncSentCount_; }
  bool prepareTracklog(const char* filepath);
  bool prepareSyncAll();
  bool hasPreparedTracklog() const { return preparedTracklog_; }
  bool syncAllActive() const { return syncAllActive_; }
  bool canCancelPending() const;
  void cancelPending();

 private:
  static constexpr uint8_t kPageSize = 5;
  static constexpr size_t kChunkSize = 20;
  static constexpr uint32_t kNotifyIntervalMs = 55UL;
  static constexpr uint32_t kAdvertisingAutoOffMs = 120000UL;
  static constexpr uint32_t kConnectedIdleAutoOffMs = 300000UL;
  static constexpr uint32_t kAutoStartSyncDelayMs = 1200UL;
  static constexpr uint32_t kFinalStatusHoldMs = 4000UL;
  static constexpr uint32_t kCancelledStatusHoldMs = 350UL;
  static constexpr uint32_t kCatalogNotifyIntervalMs = 45UL;
  static constexpr uint32_t kFileAckTimeoutMs = 12000UL;
  static constexpr uint32_t kInterFilePauseMs = 300UL;
  static constexpr uint32_t kDeferredCommandDelayMs = 300UL;
  static constexpr size_t kCommandBufferSize = 160;
  static constexpr uint8_t kSyncQueueMax = 96;

  enum class DeferredCommand : uint8_t {
    None,
    SyncNone,
    SyncSelection,
    FileOk,
    SkipSelected,
    GetSelected,
    GetIndex,
  };

  FlightRecorder* recorder_ = nullptr;
  BLEServer* server_ = nullptr;
  BLECharacteristic* commandChar_ = nullptr;
  BLECharacteristic* statusChar_ = nullptr;
  BLECharacteristic* dataChar_ = nullptr;
  TracklogBleServerCallbacks* serverCallbacks_ = nullptr;
  TracklogBleCommandCallbacks* commandCallbacks_ = nullptr;
  bool ready_ = false;
  bool bleWindowActive_ = false;
  bool connected_ = false;
  bool transferActive_ = false;
  bool syncAllPrepared_ = false;
  bool syncAllActive_ = false;
  bool syncSelectionPending_ = false;
  bool syncUseQueue_ = false;
  bool syncCatalogActive_ = false;
  bool syncWaitingFileAck_ = false;
  bool syncSelectedExport_ = false;
  bool syncAutoStartPending_ = false;
  bool restartAdvertising_ = false;
  DeferredCommand deferredCommand_ = DeferredCommand::None;
  uint16_t transferIndex_ = 0;
  uint16_t deferredGetIndex_ = 0;
  uint16_t preparedIndex_ = 0;
  uint16_t syncTotal_ = 0;
  uint16_t syncSentCount_ = 0;
  uint16_t syncCatalogIndex_ = 0;
  uint32_t transferOffset_ = 0;
  uint32_t transferSize_ = 0;
  uint32_t lastNotifyMs_ = 0;
  uint32_t lastCatalogNotifyMs_ = 0;
  uint32_t syncFileAckDeadlineMs_ = 0;
  uint32_t syncNextTransferAtMs_ = 0;
  uint32_t deferredCommandAtMs_ = 0;
  uint32_t lastBleActivityMs_ = 0;
  uint32_t connectedAtMs_ = 0;
  uint32_t statusHideAtMs_ = 0;
  char transferPath_[64] = {};
  char transferName_[40] = {};
  char statusText_[64] = {};
  char preparedPath_[64] = {};
  char preparedName_[40] = {};
  char deferredSyncSelection_[kCommandBufferSize] = {};
  uint32_t preparedSize_ = 0;
  bool preparedTracklog_ = false;
  bool preparedIndexValid_ = false;
  uint16_t syncQueue_[kSyncQueueMax] = {};
  uint16_t syncQueueCount_ = 0;
  uint8_t chunk_[kChunkSize] = {};

  bool startBle();
  void shutdownBle();
  void noteBleActivity();
  void onClientConnected();
  void onClientDisconnected();
  void handleCommand(const char* command, size_t length);
  void sendList(uint16_t page);
  void sendSyncCatalog();
  void sendNextCatalogEntry();
  void startTransfer(uint16_t index);
  void startPreparedTransfer();
  void startPreparedSyncTransfer();
  void startSyncAll();
  void startSyncSelection(const char* indicesText);
  void startNextSyncTransfer();
  void acknowledgeSyncFile();
  void queueDeferredCommand(DeferredCommand command, const char* payload = nullptr);
  bool processDeferredCommand();
  void cancelTransfer();
  void cancelSyncAll();
  void finishCurrentTransfer();
  void sendNextChunk();
  void notifyStatus(const char* text);
  void notifyStatusf(const char* fmt, ...);
  void setStatusText(const char* text);
  void setStatusTextf(const char* fmt, ...);
  void holdStatus(uint32_t holdMs = kFinalStatusHoldMs);
  void clearStatusHold();
  bool loadPreparedTracklog(const char* filepath);
  bool prepareSyncCatalog();
  bool parseSyncQueue(const char* indicesText);
  bool findTracklogIndex(const char* filepath, const char* name, uint32_t sizeBytes, uint16_t& index) const;
  static bool parseUnsigned(const char* text, uint16_t& value);
  static void copyFileName(const char* path, char* out, size_t outSize);

  friend class TracklogBleCommandCallbacks;
  friend class TracklogBleServerCallbacks;
};
