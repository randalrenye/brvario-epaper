#include "ble/TracklogBleService.h"

#include <BLE2902.h>
#include <BLEAdvertising.h>
#include <BLECharacteristic.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEService.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "tracklog/FlightRecorder.h"

namespace {

static const char* kDeviceName = "BRVARIO-E-PAPER";
static const char* kServiceUuid = "b7a10000-5a4d-4c8a-9bb7-0c2a7f9b0001";
static const char* kCommandUuid = "b7a10001-5a4d-4c8a-9bb7-0c2a7f9b0001";
static const char* kStatusUuid = "b7a10002-5a4d-4c8a-9bb7-0c2a7f9b0001";
static const char* kDataUuid = "b7a10003-5a4d-4c8a-9bb7-0c2a7f9b0001";
static const char* kWaitingAppStatus = "AGUARDANDO APP DO BRVARIO";

void trimCommand(char* text) {
  if (!text) {
    return;
  }
  size_t len = strlen(text);
  while (len > 0 && (text[len - 1] == '\r' || text[len - 1] == '\n' || text[len - 1] == ' ' || text[len - 1] == '\t')) {
    text[--len] = '\0';
  }
}

}  // namespace

class TracklogBleServerCallbacks : public BLEServerCallbacks {
 public:
  explicit TracklogBleServerCallbacks(TracklogBleService* owner) : owner_(owner) {}

  void onConnect(BLEServer*) override {
    if (owner_) {
      owner_->onClientConnected();
    }
  }

  void onDisconnect(BLEServer*) override {
    if (owner_) {
      owner_->onClientDisconnected();
    }
  }

 private:
  TracklogBleService* owner_;
};

class TracklogBleCommandCallbacks : public BLECharacteristicCallbacks {
 public:
  explicit TracklogBleCommandCallbacks(TracklogBleService* owner) : owner_(owner) {}

  void onWrite(BLECharacteristic* characteristic) override {
    if (!owner_ || !characteristic) {
      return;
    }
    std::string value = characteristic->getValue();
    owner_->handleCommand(value.data(), value.size());
  }

 private:
  TracklogBleService* owner_;
};

TracklogBleService::TracklogBleService() = default;

bool TracklogBleService::begin(FlightRecorder* recorder) {
  if (!recorder) {
    return false;
  }
  recorder_ = recorder;
  ready_ = false;
  bleWindowActive_ = false;
  connected_ = false;
  transferActive_ = false;
  syncAllPrepared_ = false;
  syncAllActive_ = false;
  syncSelectionPending_ = false;
  syncUseQueue_ = false;
  syncCatalogActive_ = false;
  syncWaitingFileAck_ = false;
  syncSelectedExport_ = false;
  syncAutoStartPending_ = false;
  deferredCommand_ = DeferredCommand::None;
  deferredGetIndex_ = 0;
  syncQueueCount_ = 0;
  syncCatalogIndex_ = 0;
  lastCatalogNotifyMs_ = 0;
  syncFileAckDeadlineMs_ = 0;
  syncNextTransferAtMs_ = 0;
  deferredCommandAtMs_ = 0;
  deferredSyncSelection_[0] = '\0';
  restartAdvertising_ = false;
  preparedTracklog_ = false;
  preparedIndexValid_ = false;
  preparedIndex_ = 0;
  preparedPath_[0] = '\0';
  preparedName_[0] = '\0';
  preparedSize_ = 0;
  setStatusText("BLE DESLIGADO");
  Serial.println("BLE Tracklog em espera. Sera ligado pelo botao EXPORTAR.");
  return true;
}

bool TracklogBleService::startBle() {
  clearStatusHold();
  if (ready_) {
    if (!server_ || !commandChar_ || !statusChar_ || !dataChar_) {
      Serial.println("BLE Tracklog: estado interno invalido, reinicializando BLE.");
      ready_ = false;
      bleWindowActive_ = false;
    } else {
      bleWindowActive_ = true;
      noteBleActivity();
      if (!connected_) {
        BLEDevice::startAdvertising();
      }
      setStatusText(kWaitingAppStatus);
      return true;
    }
  }

  setStatusText("LIGANDO BLUETOOTH");
  BLEDevice::init(kDeviceName);
  BLEDevice::setMTU(185);

  server_ = BLEDevice::createServer();
  if (!server_) {
    return false;
  }
  if (!serverCallbacks_) {
    serverCallbacks_ = new TracklogBleServerCallbacks(this);
  }
  server_->setCallbacks(serverCallbacks_);

  BLEService* service = server_->createService(kServiceUuid);
  if (!service) {
    return false;
  }

  commandChar_ = service->createCharacteristic(kCommandUuid, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  statusChar_ = service->createCharacteristic(kStatusUuid, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  dataChar_ = service->createCharacteristic(kDataUuid, BLECharacteristic::PROPERTY_NOTIFY);
  if (!commandChar_ || !statusChar_ || !dataChar_) {
    return false;
  }

  if (!commandCallbacks_) {
    commandCallbacks_ = new TracklogBleCommandCallbacks(this);
  }
  commandChar_->setCallbacks(commandCallbacks_);
  statusChar_->addDescriptor(new BLE2902());
  dataChar_->addDescriptor(new BLE2902());
  const char* readyStatus = "READY:BRVARIO-E-PAPER";
  statusChar_->setValue(reinterpret_cast<uint8_t*>(const_cast<char*>(readyStatus)), strlen(readyStatus));

  service->start();
  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  ready_ = true;
  bleWindowActive_ = true;
  connected_ = false;
  transferActive_ = false;
  syncAllActive_ = false;
  syncSelectionPending_ = false;
  syncUseQueue_ = false;
  syncCatalogActive_ = false;
  syncWaitingFileAck_ = false;
  syncSelectedExport_ = false;
  syncAutoStartPending_ = false;
  deferredCommand_ = DeferredCommand::None;
  deferredGetIndex_ = 0;
  syncCatalogIndex_ = 0;
  lastCatalogNotifyMs_ = 0;
  syncFileAckDeadlineMs_ = 0;
  syncNextTransferAtMs_ = 0;
  deferredCommandAtMs_ = 0;
  deferredSyncSelection_[0] = '\0';
  restartAdvertising_ = false;
  noteBleActivity();
  setStatusText(kWaitingAppStatus);
  Serial.println("BLE Tracklog ligado: anunciando como BRVARIO-E-PAPER.");
  return true;
}

void TracklogBleService::update() {
  if (!ready_) {
    return;
  }

  const uint32_t now = millis();

  if (restartAdvertising_) {
    restartAdvertising_ = false;
    BLEDevice::startAdvertising();
    Serial.println("BLE Tracklog: advertising reiniciado.");
  }

  if (connected_ && syncAutoStartPending_ && syncAllPrepared_ && !syncAllActive_ && !transferActive_ &&
      now - connectedAtMs_ >= kAutoStartSyncDelayMs) {
    syncAutoStartPending_ = false;
    sendSyncCatalog();
    return;
  }

  if (connected_ && syncCatalogActive_) {
    sendNextCatalogEntry();
    return;
  }

  if (connected_ && processDeferredCommand()) {
    return;
  }

  if (connected_ && syncWaitingFileAck_) {
    if (syncFileAckDeadlineMs_ != 0 && static_cast<int32_t>(now - syncFileAckDeadlineMs_) >= 0) {
      Serial.println("BLE Tracklog: timeout aguardando FILE_OK; continuando sync.");
      acknowledgeSyncFile();
    }
    return;
  }

  if (connected_ && syncAllActive_ && !transferActive_ && syncNextTransferAtMs_ != 0 &&
      static_cast<int32_t>(now - syncNextTransferAtMs_) >= 0) {
    syncNextTransferAtMs_ = 0;
    startNextSyncTransfer();
    return;
  }

  if (connected_ && transferActive_) {
    sendNextChunk();
    return;
  }

  const uint32_t timeoutMs = connected_ ? kConnectedIdleAutoOffMs : kAdvertisingAutoOffMs;
  if (lastBleActivityMs_ != 0 && now - lastBleActivityMs_ >= timeoutMs) {
    Serial.println("BLE Tracklog: desligado automaticamente por inatividade.");
    shutdownBle();
  }
}

void TracklogBleService::end() {
  shutdownBle();
}

void TracklogBleService::shutdownBle() {
  if (!ready_) {
    return;
  }

  cancelTransfer();
  cancelSyncAll();
  BLEDevice::getAdvertising()->stop();
  if (connected_ && server_) {
    server_->disconnect(server_->getConnId());
  }
  connected_ = false;
  restartAdvertising_ = false;
  bleWindowActive_ = false;
  syncAutoStartPending_ = false;
  preparedTracklog_ = false;
  preparedIndexValid_ = false;
  preparedIndex_ = 0;
  preparedPath_[0] = '\0';
  preparedName_[0] = '\0';
  preparedSize_ = 0;
  lastBleActivityMs_ = 0;
  connectedAtMs_ = 0;
  statusHideAtMs_ = 0;
  setStatusText("BLE DESLIGADO");
}

void TracklogBleService::noteBleActivity() {
  lastBleActivityMs_ = millis();
}

void TracklogBleService::onClientConnected() {
  clearStatusHold();
  connected_ = true;
  bleWindowActive_ = true;
  restartAdvertising_ = false;
  connectedAtMs_ = millis();
  noteBleActivity();
  setStatusText("APP CONECTADO");
  Serial.println("BLE Tracklog: app conectado.");
  notifyStatus("CONNECTED:BRVARIO-E-PAPER");
  if (preparedTracklog_) {
    setStatusText("VERIFICANDO APP");
    notifyStatusf("SELECTED_READY:%lu:%s", static_cast<unsigned long>(preparedSize_), preparedName_);
  }
  if (syncAllPrepared_) {
    syncAutoStartPending_ = true;
    setStatusText("PREPARANDO LISTA");
    notifyStatusf("SYNC_READY:%u", static_cast<unsigned>(syncTotal_));
  }
}

void TracklogBleService::onClientDisconnected() {
  connected_ = false;
  connectedAtMs_ = 0;
  cancelTransfer();
  syncAllActive_ = false;
  syncCatalogActive_ = false;
  syncAutoStartPending_ = false;
  syncCatalogIndex_ = 0;
  lastCatalogNotifyMs_ = 0;
  restartAdvertising_ = true;
  noteBleActivity();
  setStatusText(kWaitingAppStatus);
  Serial.println("BLE Tracklog: app desconectado.");
}

void TracklogBleService::handleCommand(const char* command, size_t length) {
  if (!command || length == 0) {
    return;
  }

  char cmd[kCommandBufferSize];
  const size_t copyLen = length < sizeof(cmd) - 1 ? length : sizeof(cmd) - 1;
  memcpy(cmd, command, copyLen);
  cmd[copyLen] = '\0';
  trimCommand(cmd);

  Serial.print("BLE Tracklog comando: ");
  Serial.println(cmd);
  noteBleActivity();

  if (strcmp(cmd, "PING") == 0) {
    notifyStatus("PONG:BRVARIO-E-PAPER");
    return;
  }
  if (strcmp(cmd, "INFO") == 0) {
    notifyStatus("INFO:BRVARIO-E-PAPER:IGC_BLE:1:MTU185");
    return;
  }
  if (strcmp(cmd, "CANCEL") == 0) {
    if (canCancelPending()) {
      cancelPending();
      notifyStatus("CANCELLED");
    } else {
      setStatusText("ENVIO EM ANDAMENTO");
      holdStatus();
      notifyStatus("ERROR:CANNOT_CANCEL_ACTIVE_TRANSFER");
    }
    return;
  }
  if (strcmp(cmd, "FILE_OK") == 0) {
    queueDeferredCommand(DeferredCommand::FileOk);
    return;
  }
  if (strcmp(cmd, "SKIP_SELECTED") == 0) {
    queueDeferredCommand(DeferredCommand::SkipSelected);
    return;
  }
  if (strcmp(cmd, "SYNC_ALL") == 0) {
    if (syncCatalogActive_) {
      setStatusText("CATALOGO EM ANDAMENTO");
      notifyStatus("SYNC_CATALOG_BUSY");
      return;
    }
    if (!prepareSyncCatalog()) {
      notifyStatus("ERROR:NO_TRACKLOGS");
      holdStatus();
      return;
    }
    syncAutoStartPending_ = true;
    connectedAtMs_ = millis();
    setStatusText("PREPARANDO LISTA");
    notifyStatusf("SYNC_READY:%u", static_cast<unsigned>(syncTotal_));
    return;
  }
  if (strcmp(cmd, "SYNC_FORCE_ALL") == 0) {
    if (!prepareSyncCatalog()) {
      notifyStatus("ERROR:NO_TRACKLOGS");
      holdStatus();
      return;
    }
    startSyncAll();
    return;
  }
  if (strcmp(cmd, "SYNC_NONE") == 0 || strcmp(cmd, "SYNC_MISSING:") == 0) {
    setStatusText("FINALIZANDO SYNC");
    queueDeferredCommand(DeferredCommand::SyncNone);
    return;
  }
  if (strncmp(cmd, "SYNC_MISSING:", 13) == 0) {
    setStatusText("PREPARANDO ENVIO");
    queueDeferredCommand(DeferredCommand::SyncSelection, &cmd[13]);
    return;
  }
  if (strcmp(cmd, "SELECTED") == 0) {
    if (preparedTracklog_) {
      notifyStatusf("SELECTED:%lu:%s", static_cast<unsigned long>(preparedSize_), preparedName_);
    } else {
      notifyStatus("ERROR:NO_SELECTED");
    }
    return;
  }
  if (strncmp(cmd, "LIST", 4) == 0) {
    uint16_t page = 0;
    if (cmd[4] == ':' && !parseUnsigned(&cmd[5], page)) {
      notifyStatus("ERROR:BAD_PAGE");
      return;
    }
    sendList(page);
    return;
  }
  if (strncmp(cmd, "GET:", 4) == 0) {
    if (strcmp(&cmd[4], "SELECTED") == 0 || strcmp(&cmd[4], "READY") == 0) {
      setStatusText("PREPARANDO ARQUIVO");
      queueDeferredCommand(DeferredCommand::GetSelected);
      return;
    }
    uint16_t index = 0;
    if (!parseUnsigned(&cmd[4], index)) {
      notifyStatus("ERROR:BAD_INDEX");
      return;
    }
    deferredGetIndex_ = index;
    setStatusText("PREPARANDO ARQUIVO");
    queueDeferredCommand(DeferredCommand::GetIndex);
    return;
  }

  notifyStatus("ERROR:UNKNOWN_COMMAND");
}

bool TracklogBleService::prepareTracklog(const char* filepath) {
  clearStatusHold();
  if (transferActive_ || syncAllActive_) {
    return false;
  }
  if (!loadPreparedTracklog(filepath)) {
    setStatusText("FALHA AO LER IGC");
    return false;
  }
  if (!startBle()) {
    preparedTracklog_ = false;
    setStatusText("FALHA BLE");
    return false;
  }

  Serial.print("BLE Tracklog selecionado para exportacao: ");
  Serial.println(preparedPath_);
  if (connected_) {
    setStatusText("VERIFICANDO APP");
    notifyStatusf("SELECTED_READY:%lu:%s", static_cast<unsigned long>(preparedSize_), preparedName_);
  } else {
    setStatusText(kWaitingAppStatus);
    notifyStatusf("SELECTED_READY:%lu:%s", static_cast<unsigned long>(preparedSize_), preparedName_);
  }
  return true;
}

bool TracklogBleService::prepareSyncAll() {
  clearStatusHold();
  if (!recorder_ || !recorder_->storageReady() || transferActive_ || syncAllActive_) {
    setStatusText("BLE OCUPADO");
    return false;
  }

  syncTotal_ = recorder_->tracklogCount();
  if (syncTotal_ == 0) {
    setStatusText("SEM TRACKLOGS");
    return false;
  }

  syncSentCount_ = 0;
  syncAllPrepared_ = true;
  syncSelectionPending_ = true;
  syncUseQueue_ = false;
  syncQueueCount_ = 0;
  preparedTracklog_ = false;
  preparedIndexValid_ = false;
  preparedIndex_ = 0;
  if (!startBle()) {
    syncAllPrepared_ = false;
    setStatusText("FALHA BLE");
    return false;
  }

  Serial.print("BLE Tracklog: sincronizacao de todos os arquivos preparada. Total=");
  Serial.println(syncTotal_);
  if (connected_) {
    syncAutoStartPending_ = true;
    connectedAtMs_ = millis();
    setStatusText("PREPARANDO LISTA");
    notifyStatusf("SYNC_READY:%u", static_cast<unsigned>(syncTotal_));
  } else {
    setStatusText(kWaitingAppStatus);
    notifyStatusf("SYNC_READY:%u", static_cast<unsigned>(syncTotal_));
  }
  return true;
}

bool TracklogBleService::canCancelPending() const {
  if (!visibleStatus()) {
    return false;
  }
  if (transferActive_ || syncAllActive_ || syncCatalogActive_ || syncWaitingFileAck_) {
    return false;
  }
  return enabled() || preparedTracklog_ || syncAllPrepared_ || syncSelectionPending_ || syncAutoStartPending_;
}

void TracklogBleService::cancelPending() {
  if (!canCancelPending()) {
    setStatusText("ENVIO EM ANDAMENTO");
    holdStatus();
    return;
  }

  shutdownBle();
  setStatusText("OPERAÇAO CANCELADA");
  holdStatus(kCancelledStatusHoldMs);
}

void TracklogBleService::sendList(uint16_t page) {
  noteBleActivity();
  if (!recorder_ || !recorder_->storageReady()) {
    setStatusText("ERRO MEMORIA");
    notifyStatus("ERROR:STORAGE");
    return;
  }

  FlightRecorder::TracklogEntry entries[kPageSize];
  uint8_t entryCount = 0;
  uint16_t totalCount = 0;
  uint32_t usedBytes = 0;
  if (!recorder_->tracklogPage(page, kPageSize, entries, entryCount, totalCount, usedBytes)) {
    notifyStatus("ERROR:LIST");
    return;
  }

  notifyStatusf("LIST_BEGIN:%u:%u:%u:%lu",
                static_cast<unsigned>(page),
                static_cast<unsigned>(totalCount),
                static_cast<unsigned>(entryCount),
                static_cast<unsigned long>(usedBytes));
  for (uint8_t i = 0; i < entryCount; ++i) {
    const uint16_t globalIndex = static_cast<uint16_t>(page * kPageSize + i);
    notifyStatusf("ENTRY:%u:%lu:%s:%s:%s",
                  static_cast<unsigned>(globalIndex),
                  static_cast<unsigned long>(entries[i].sizeBytes),
                  entries[i].displayDate,
                  entries[i].displayTime,
                  entries[i].name);
    delay(6);
  }
  notifyStatus("LIST_END");
}

bool TracklogBleService::prepareSyncCatalog() {
  if (!recorder_ || !recorder_->storageReady()) {
    setStatusText("ERRO MEMORIA");
    return false;
  }

  syncTotal_ = recorder_->tracklogCount();
  if (syncTotal_ == 0) {
    setStatusText("SEM TRACKLOGS");
    cancelSyncAll();
    return false;
  }

  syncSentCount_ = 0;
  syncQueueCount_ = 0;
  syncUseQueue_ = false;
  syncAllActive_ = false;
  syncAllPrepared_ = true;
  syncSelectionPending_ = true;
  return true;
}

void TracklogBleService::sendSyncCatalog() {
  noteBleActivity();
  syncAutoStartPending_ = false;
  if (syncCatalogActive_) {
    return;
  }
  if (!prepareSyncCatalog()) {
    notifyStatus("ERROR:NO_TRACKLOGS");
    holdStatus();
    return;
  }

  if (!connected_) {
    setStatusText(kWaitingAppStatus);
    notifyStatusf("SYNC_READY:%u", static_cast<unsigned>(syncTotal_));
    return;
  }

  setStatusText("COMPARANDO ARQUIVOS");
  notifyStatusf("SYNC_CATALOG_BEGIN:%u", static_cast<unsigned>(syncTotal_));
  syncCatalogIndex_ = 0;
  lastCatalogNotifyMs_ = 0;
  syncCatalogActive_ = true;
}

void TracklogBleService::sendNextCatalogEntry() {
  if (!syncCatalogActive_) {
    return;
  }

  const uint32_t now = millis();
  if (lastCatalogNotifyMs_ != 0 && now - lastCatalogNotifyMs_ < kCatalogNotifyIntervalMs) {
    return;
  }
  lastCatalogNotifyMs_ = now;

  if (!recorder_ || !recorder_->storageReady()) {
    setStatusText("ERRO MEMORIA");
    notifyStatus("ERROR:STORAGE");
    syncCatalogActive_ = false;
    cancelSyncAll();
    holdStatus();
    return;
  }

  if (syncCatalogIndex_ >= syncTotal_) {
    notifyStatus("SYNC_CATALOG_END");
    syncCatalogActive_ = false;
    setStatusText(kWaitingAppStatus);
    Serial.println("BLE Tracklog: catalogo finalizado; aguardando resposta do app.");
    noteBleActivity();
    return;
  }

  FlightRecorder::TracklogEntry entry;
  if (!recorder_->tracklogEntry(syncCatalogIndex_, entry)) {
    setStatusText("ERRO LISTA IGC");
    notifyStatus("ERROR:CATALOG");
    syncCatalogActive_ = false;
    cancelSyncAll();
    holdStatus();
    return;
  }

  notifyStatusf("SYNC_ENTRY:%u:%lu:%s:%s:%s",
                static_cast<unsigned>(syncCatalogIndex_),
                static_cast<unsigned long>(entry.sizeBytes),
                entry.displayDate,
                entry.displayTime,
                entry.name);
  ++syncCatalogIndex_;
  noteBleActivity();
}

void TracklogBleService::startTransfer(uint16_t index) {
  clearStatusHold();
  noteBleActivity();
  if (!recorder_ || !recorder_->storageReady()) {
    setStatusText("ERRO MEMORIA");
    notifyStatus("ERROR:STORAGE");
    holdStatus();
    return;
  }
  if (transferActive_) {
    setStatusText("BLE OCUPADO");
    notifyStatus("ERROR:BUSY");
    return;
  }

  FlightRecorder::TracklogEntry entry;
  if (!recorder_->tracklogEntry(index, entry)) {
    setStatusText("ARQUIVO NAO ENCONTRADO");
    notifyStatus("ERROR:NOT_FOUND");
    cancelSyncAll();
    holdStatus();
    return;
  }

  snprintf(transferPath_, sizeof(transferPath_), "%s", entry.path);
  snprintf(transferName_, sizeof(transferName_), "%s", entry.name);
  transferIndex_ = index;
  transferOffset_ = 0;
  transferSize_ = entry.sizeBytes;
  transferActive_ = true;
  lastNotifyMs_ = 0;
  if (syncAllActive_) {
    setStatusTextf("ENVIANDO %u/%u", static_cast<unsigned>(syncSentCount_ + 1), static_cast<unsigned>(syncTotal_));
  } else {
    setStatusText("ENVIANDO ARQUIVO");
  }

  notifyStatusf("FILE_BEGIN:%u:%lu:%s",
                static_cast<unsigned>(transferIndex_),
                static_cast<unsigned long>(transferSize_),
                transferName_);
}

void TracklogBleService::startPreparedTransfer() {
  clearStatusHold();
  noteBleActivity();
  if (!preparedTracklog_) {
    setStatusText("NENHUM ARQUIVO");
    notifyStatus("ERROR:NO_SELECTED");
    holdStatus();
    return;
  }
  if (transferActive_) {
    setStatusText("BLE OCUPADO");
    notifyStatus("ERROR:BUSY");
    return;
  }

  snprintf(transferPath_, sizeof(transferPath_), "%s", preparedPath_);
  snprintf(transferName_, sizeof(transferName_), "%s", preparedName_);
  transferIndex_ = UINT16_MAX;
  transferOffset_ = 0;
  transferSize_ = preparedSize_;
  transferActive_ = true;
  lastNotifyMs_ = 0;
  setStatusText("ENVIANDO ARQUIVO");
  notifyStatusf("FILE_BEGIN:%u:%lu:%s",
                static_cast<unsigned>(transferIndex_),
                static_cast<unsigned long>(transferSize_),
                transferName_);
}

void TracklogBleService::startPreparedSyncTransfer() {
  clearStatusHold();
  noteBleActivity();
  if (!preparedTracklog_ || !preparedIndexValid_) {
    setStatusText("NENHUM ARQUIVO");
    notifyStatus("ERROR:NO_SELECTED");
    holdStatus();
    return;
  }
  if (transferActive_ || syncAllActive_) {
    setStatusText("BLE OCUPADO");
    notifyStatus("ERROR:BUSY");
    return;
  }

  syncQueue_[0] = preparedIndex_;
  syncQueueCount_ = 1;
  syncUseQueue_ = true;
  syncSelectedExport_ = true;
  syncSelectionPending_ = false;
  syncAllPrepared_ = true;
  syncAllActive_ = true;
  syncWaitingFileAck_ = false;
  syncFileAckDeadlineMs_ = 0;
  syncNextTransferAtMs_ = 0;
  syncSentCount_ = 0;
  syncTotal_ = 1;
  setStatusText("INICIANDO EXPORT");
  notifyStatus("SYNC_BEGIN:1");
  startNextSyncTransfer();
}

void TracklogBleService::startSyncAll() {
  clearStatusHold();
  noteBleActivity();
  syncAutoStartPending_ = false;
  syncCatalogActive_ = false;
  syncCatalogIndex_ = 0;
  lastCatalogNotifyMs_ = 0;
  if (!recorder_ || !recorder_->storageReady()) {
    setStatusText("ERRO MEMORIA");
    notifyStatus("ERROR:STORAGE");
    holdStatus();
    return;
  }
  if (transferActive_) {
    setStatusText("BLE OCUPADO");
    notifyStatus("ERROR:BUSY");
    return;
  }

  if (!syncAllPrepared_) {
    syncTotal_ = recorder_->tracklogCount();
    syncAllPrepared_ = syncTotal_ > 0;
  }
  if (syncTotal_ == 0) {
    setStatusText("SEM TRACKLOGS");
    notifyStatus("ERROR:NO_TRACKLOGS");
    cancelSyncAll();
    holdStatus();
    return;
  }

  syncSelectionPending_ = false;
  syncUseQueue_ = false;
  syncQueueCount_ = 0;
  syncAllActive_ = true;
  syncWaitingFileAck_ = false;
  syncFileAckDeadlineMs_ = 0;
  syncNextTransferAtMs_ = 0;
  syncSentCount_ = 0;
  setStatusText("INICIANDO SYNC");
  notifyStatusf("SYNC_BEGIN:%u", static_cast<unsigned>(syncTotal_));
  startNextSyncTransfer();
}

void TracklogBleService::startSyncSelection(const char* indicesText) {
  clearStatusHold();
  noteBleActivity();
  syncCatalogActive_ = false;
  syncCatalogIndex_ = 0;
  lastCatalogNotifyMs_ = 0;
  if (transferActive_) {
    setStatusText("BLE OCUPADO");
    notifyStatus("ERROR:BUSY");
    return;
  }
  if ((!syncAllPrepared_ || syncTotal_ == 0) && !prepareSyncCatalog()) {
    notifyStatus("ERROR:NO_TRACKLOGS");
    holdStatus();
    return;
  }
  if (!parseSyncQueue(indicesText)) {
    setStatusText("LISTA INVALIDA");
    notifyStatus("ERROR:BAD_SYNC_SELECTION");
    cancelSyncAll();
    holdStatus();
    return;
  }
  if (syncQueueCount_ == 0) {
    cancelSyncAll();
    setStatusText("TUDO SINCRONIZADO");
    notifyStatus("SYNC_END:0");
    holdStatus();
    return;
  }

  syncUseQueue_ = true;
  syncSelectionPending_ = false;
  syncAllPrepared_ = true;
  syncAllActive_ = true;
  syncWaitingFileAck_ = false;
  syncFileAckDeadlineMs_ = 0;
  syncNextTransferAtMs_ = 0;
  syncSentCount_ = 0;
  syncTotal_ = syncQueueCount_;
  setStatusText("INICIANDO SYNC");
  notifyStatusf("SYNC_BEGIN:%u", static_cast<unsigned>(syncTotal_));
  startNextSyncTransfer();
}

void TracklogBleService::startNextSyncTransfer() {
  if (!syncAllActive_ || syncSentCount_ >= syncTotal_) {
    return;
  }

  const uint16_t index = syncUseQueue_ ? syncQueue_[syncSentCount_] : syncSentCount_;
  startTransfer(index);
}

void TracklogBleService::cancelTransfer() {
  transferActive_ = false;
  transferOffset_ = 0;
  transferSize_ = 0;
  transferPath_[0] = '\0';
  transferName_[0] = '\0';
}

void TracklogBleService::cancelSyncAll() {
  syncAllActive_ = false;
  syncAllPrepared_ = false;
  syncSelectionPending_ = false;
  syncUseQueue_ = false;
  syncCatalogActive_ = false;
  syncWaitingFileAck_ = false;
  syncSelectedExport_ = false;
  syncAutoStartPending_ = false;
  deferredCommand_ = DeferredCommand::None;
  deferredGetIndex_ = 0;
  syncTotal_ = 0;
  syncSentCount_ = 0;
  syncCatalogIndex_ = 0;
  lastCatalogNotifyMs_ = 0;
  syncFileAckDeadlineMs_ = 0;
  syncNextTransferAtMs_ = 0;
  deferredCommandAtMs_ = 0;
  deferredSyncSelection_[0] = '\0';
  syncQueueCount_ = 0;
}

void TracklogBleService::finishCurrentTransfer() {
  notifyStatusf("FILE_END:%u:%lu",
                static_cast<unsigned>(transferIndex_),
                static_cast<unsigned long>(transferOffset_));
  cancelTransfer();

  if (!syncAllActive_) {
    setStatusText("ENVIO CONCLUIDO");
    holdStatus();
    return;
  }

  syncWaitingFileAck_ = true;
  syncFileAckDeadlineMs_ = millis() + kFileAckTimeoutMs;
  setStatusText("SALVANDO NO APP");
}

void TracklogBleService::acknowledgeSyncFile() {
  if (!syncAllActive_ || !syncWaitingFileAck_) {
    return;
  }

  syncWaitingFileAck_ = false;
  syncFileAckDeadlineMs_ = 0;
  ++syncSentCount_;
  if (syncSentCount_ >= syncTotal_) {
    if (syncSelectedExport_) {
      preparedTracklog_ = false;
      preparedIndexValid_ = false;
      preparedIndex_ = 0;
      preparedPath_[0] = '\0';
      preparedName_[0] = '\0';
      preparedSize_ = 0;
    }
    setStatusText("SYNC CONCLUIDA");
    notifyStatusf("SYNC_END:%u", static_cast<unsigned>(syncSentCount_));
    cancelSyncAll();
    holdStatus();
    return;
  }

  setStatusText("PROXIMO ARQUIVO");
  syncNextTransferAtMs_ = millis() + kInterFilePauseMs;
}

void TracklogBleService::queueDeferredCommand(DeferredCommand command, const char* payload) {
  deferredCommand_ = command;
  deferredCommandAtMs_ = millis() + kDeferredCommandDelayMs;
  if (payload) {
    snprintf(deferredSyncSelection_, sizeof(deferredSyncSelection_), "%s", payload);
  } else {
    deferredSyncSelection_[0] = '\0';
  }
}

bool TracklogBleService::processDeferredCommand() {
  if (deferredCommand_ == DeferredCommand::None || deferredCommandAtMs_ == 0) {
    return false;
  }

  const uint32_t now = millis();
  if (static_cast<int32_t>(now - deferredCommandAtMs_) < 0) {
    return false;
  }

  const DeferredCommand command = deferredCommand_;
  const uint16_t getIndex = deferredGetIndex_;
  char payload[kCommandBufferSize];
  snprintf(payload, sizeof(payload), "%s", deferredSyncSelection_);
  deferredCommand_ = DeferredCommand::None;
  deferredGetIndex_ = 0;
  deferredCommandAtMs_ = 0;
  deferredSyncSelection_[0] = '\0';

  switch (command) {
    case DeferredCommand::SyncNone:
      Serial.println("BLE Tracklog: app informou que nao ha arquivos faltantes.");
      syncCatalogActive_ = false;
      cancelSyncAll();
      setStatusText("TUDO SINCRONIZADO");
      notifyStatus("SYNC_END:0");
      holdStatus();
      return true;

    case DeferredCommand::SyncSelection:
      Serial.print("BLE Tracklog: iniciando envio dos indices faltantes: ");
      Serial.println(payload);
      startSyncSelection(payload);
      return true;

    case DeferredCommand::FileOk:
      acknowledgeSyncFile();
      return true;

    case DeferredCommand::SkipSelected:
      cancelTransfer();
      preparedTracklog_ = false;
      preparedIndexValid_ = false;
      preparedIndex_ = 0;
      preparedPath_[0] = '\0';
      preparedName_[0] = '\0';
      preparedSize_ = 0;
      setStatusText("ARQUIVO JA EXISTE");
      notifyStatus("SELECTED_SKIPPED");
      holdStatus();
      return true;

    case DeferredCommand::GetSelected:
      Serial.println("BLE Tracklog: iniciando exportacao selecionada pelo caminho de sync.");
      startPreparedSyncTransfer();
      return true;

    case DeferredCommand::GetIndex:
      Serial.print("BLE Tracklog: iniciando envio do indice ");
      Serial.println(static_cast<unsigned>(getIndex));
      startTransfer(getIndex);
      return true;

    case DeferredCommand::None:
    default:
      return false;
  }
}

void TracklogBleService::sendNextChunk() {
  const uint32_t now = millis();
  if (lastNotifyMs_ != 0 && now - lastNotifyMs_ < kNotifyIntervalMs) {
    return;
  }
  lastNotifyMs_ = now;

  if (transferOffset_ >= transferSize_) {
    finishCurrentTransfer();
    return;
  }

  size_t bytesRead = 0;
  uint32_t fileSize = 0;
  if (!recorder_->readTracklogChunk(transferPath_, transferOffset_, chunk_, sizeof(chunk_), bytesRead, fileSize)) {
    setStatusText("ERRO LEITURA IGC");
    notifyStatus("ERROR:READ");
    cancelTransfer();
    cancelSyncAll();
    holdStatus();
    return;
  }

  if (fileSize != 0 && transferSize_ != fileSize) {
    transferSize_ = fileSize;
  }
  if (bytesRead == 0) {
    finishCurrentTransfer();
    return;
  }

  dataChar_->setValue(chunk_, bytesRead);
  dataChar_->notify();
  transferOffset_ += static_cast<uint32_t>(bytesRead);
  noteBleActivity();
  yield();
}

void TracklogBleService::notifyStatus(const char* text) {
  if (!statusChar_ || !text) {
    return;
  }
  statusChar_->setValue(reinterpret_cast<uint8_t*>(const_cast<char*>(text)), strlen(text));
  if (connected_) {
    statusChar_->notify();
  }
}

void TracklogBleService::notifyStatusf(const char* fmt, ...) {
  char text[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  notifyStatus(text);
}

void TracklogBleService::setStatusText(const char* text) {
  snprintf(statusText_, sizeof(statusText_), "%s", text ? text : "");
}

void TracklogBleService::setStatusTextf(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(statusText_, sizeof(statusText_), fmt, args);
  va_end(args);
}

bool TracklogBleService::visibleStatus() const {
  if (statusHideAtMs_ != 0) {
    return static_cast<int32_t>(millis() - statusHideAtMs_) < 0;
  }

  return enabled() || transferActive_ || syncAllPrepared_ || syncAllActive_ || preparedTracklog_;
}

void TracklogBleService::holdStatus(uint32_t holdMs) {
  if (holdMs == 0) {
    statusHideAtMs_ = 0;
    return;
  }
  statusHideAtMs_ = millis() + holdMs;
  if (statusHideAtMs_ == 0) {
    statusHideAtMs_ = 1;
  }
}

void TracklogBleService::clearStatusHold() {
  statusHideAtMs_ = 0;
}

uint8_t TracklogBleService::progressPercent() const {
  if (syncCatalogActive_ && syncTotal_ > 0) {
    uint32_t percent = (static_cast<uint32_t>(syncCatalogIndex_) * 100UL) / syncTotal_;
    if (percent > 100UL) percent = 100UL;
    return static_cast<uint8_t>(percent);
  }

  if (syncAllActive_ && syncTotal_ > 0) {
    uint32_t currentFilePercent = 0;
    if (syncWaitingFileAck_) {
      currentFilePercent = 100UL;
    } else if (transferSize_ > 0) {
      currentFilePercent = (transferOffset_ * 100UL) / transferSize_;
      if (currentFilePercent > 100UL) currentFilePercent = 100UL;
    }
    uint32_t percent = (static_cast<uint32_t>(syncSentCount_) * 100UL + currentFilePercent) / syncTotal_;
    if (percent > 100UL) percent = 100UL;
    return static_cast<uint8_t>(percent);
  }

  if (transferActive_ && transferSize_ > 0) {
    uint32_t percent = (transferOffset_ * 100UL) / transferSize_;
    if (percent > 100UL) percent = 100UL;
    return static_cast<uint8_t>(percent);
  }

  if (syncAllPrepared_ || preparedTracklog_ || enabled()) {
    return 0;
  }

  return 0;
}

bool TracklogBleService::parseSyncQueue(const char* indicesText) {
  syncQueueCount_ = 0;
  if (!indicesText) {
    return true;
  }

  const uint16_t maxIndex = recorder_ ? recorder_->tracklogCount() : syncTotal_;
  const char* cursor = indicesText;
  while (*cursor != '\0') {
    while (*cursor == ' ' || *cursor == '\t' || *cursor == ',' || *cursor == ';') {
      ++cursor;
    }
    if (*cursor == '\0') {
      break;
    }

    if (*cursor < '0' || *cursor > '9') {
      return false;
    }

    uint32_t parsed = 0;
    while (*cursor >= '0' && *cursor <= '9') {
      parsed = parsed * 10UL + static_cast<uint32_t>(*cursor - '0');
      if (parsed > UINT16_MAX) {
        return false;
      }
      ++cursor;
    }

    if (parsed >= maxIndex) {
      return false;
    }

    bool alreadyQueued = false;
    for (uint16_t i = 0; i < syncQueueCount_; ++i) {
      if (syncQueue_[i] == parsed) {
        alreadyQueued = true;
        break;
      }
    }
    if (!alreadyQueued) {
      if (syncQueueCount_ >= kSyncQueueMax) {
        return false;
      }
      syncQueue_[syncQueueCount_++] = static_cast<uint16_t>(parsed);
    }

    while (*cursor == ' ' || *cursor == '\t') {
      ++cursor;
    }
    if (*cursor != '\0' && *cursor != ',' && *cursor != ';') {
      return false;
    }
  }

  return true;
}

bool TracklogBleService::parseUnsigned(const char* text, uint16_t& value) {
  if (!text || text[0] == '\0') {
    return false;
  }
  uint32_t parsed = 0;
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor < '0' || *cursor > '9') {
      return false;
    }
    parsed = parsed * 10UL + static_cast<uint32_t>(*cursor - '0');
    if (parsed > UINT16_MAX) {
      return false;
    }
  }
  value = static_cast<uint16_t>(parsed);
  return true;
}

bool TracklogBleService::loadPreparedTracklog(const char* filepath) {
  preparedTracklog_ = false;
  preparedIndexValid_ = false;
  preparedIndex_ = 0;
  preparedPath_[0] = '\0';
  preparedName_[0] = '\0';
  preparedSize_ = 0;
  if (!recorder_ || !recorder_->storageReady() || !filepath || filepath[0] == '\0') {
    return false;
  }

  uint8_t probe[1] = {};
  size_t bytesRead = 0;
  uint32_t fileSize = 0;
  if (!recorder_->readTracklogChunk(filepath, 0, probe, sizeof(probe), bytesRead, fileSize) || fileSize == 0) {
    return false;
  }

  snprintf(preparedPath_, sizeof(preparedPath_), "%s", filepath);
  copyFileName(filepath, preparedName_, sizeof(preparedName_));
  preparedSize_ = fileSize;
  preparedIndexValid_ = findTracklogIndex(preparedPath_, preparedName_, preparedSize_, preparedIndex_);
  if (!preparedIndexValid_) {
    Serial.print("BLE Tracklog: nao encontrou indice para exportar ");
    Serial.println(preparedName_);
    preparedPath_[0] = '\0';
    preparedName_[0] = '\0';
    preparedSize_ = 0;
    return false;
  }
  preparedTracklog_ = true;
  return true;
}

bool TracklogBleService::findTracklogIndex(const char* filepath, const char* name, uint32_t sizeBytes, uint16_t& index) const {
  if (!recorder_ || !recorder_->storageReady() || !name || name[0] == '\0') {
    return false;
  }

  const uint16_t count = recorder_->tracklogCount();
  for (uint16_t i = 0; i < count; ++i) {
    FlightRecorder::TracklogEntry entry;
    if (!recorder_->tracklogEntry(i, entry)) {
      continue;
    }

    const bool sameName = strcmp(entry.name, name) == 0;
    const bool samePath = filepath && filepath[0] != '\0' && strcmp(entry.path, filepath) == 0;
    const bool sameSize = sizeBytes == 0 || entry.sizeBytes == sizeBytes;
    if ((samePath || sameName) && sameSize) {
      index = i;
      return true;
    }
  }

  return false;
}

void TracklogBleService::copyFileName(const char* path, char* out, size_t outSize) {
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
