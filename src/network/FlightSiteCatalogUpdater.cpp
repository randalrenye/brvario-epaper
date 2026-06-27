#include "network/FlightSiteCatalogUpdater.h"

#include <stdio.h>
#include <string.h>

#include "config/FlightSiteCatalogConfig.h"
#include "network/WifiManager.h"
#include "weather/FlightSiteCatalog.h"

namespace {

bool startsWithHttps(const char* url) {
  return url && strncmp(url, "https://", 8) == 0;
}

}  // namespace

constexpr char FlightSiteCatalogUpdater::kTempPath[];
constexpr char FlightSiteCatalogUpdater::kBackupPath[];

void FlightSiteCatalogUpdater::begin(fs::FS* filesystem, WifiManager* wifi) {
  filesystem_ = filesystem;
  wifi_ = wifi;
  resetStatus();
  ensureDirectory();
}

bool FlightSiteCatalogUpdater::requestUpdate() {
  if (busy()) {
    return false;
  }
  resetStatus();
  if (!filesystem_) {
    fail("LITTLEFS INDISPONIVEL");
    return false;
  }
  if (!isConfigured()) {
    fail("URL DO CATALOGO AUSENTE");
    return false;
  }
  if (!ensureDirectory()) {
    fail("PASTA /weather INDISPONIVEL");
    return false;
  }
  if (!wifi_) {
    fail("WIFI INDISPONIVEL");
    return false;
  }

  wifi_->setEnabled(true);
  if (!wifi_->isConnected()) {
    wifi_->connectSaved();
    waitStartedMs_ = millis();
    state_ = FlightSiteCatalogUpdateState::WaitingWifi;
    setStatus("AGUARDANDO WIFI");
    return true;
  }
  return startDownload();
}

void FlightSiteCatalogUpdater::update() {
  if (state_ == FlightSiteCatalogUpdateState::WaitingWifi) {
    if (wifi_ && wifi_->isConnected()) {
      startDownload();
      return;
    }
    if (millis() - waitStartedMs_ > FlightSiteCatalogConfig::kWifiWaitTimeoutMs) {
      fail("WIFI NAO CONECTOU");
    }
    return;
  }

  if (state_ != FlightSiteCatalogUpdateState::Downloading) {
    return;
  }

  WiFiClient* stream = http_.getStreamPtr();
  if (!stream) {
    fail("STREAM DO CATALOGO INVALIDO");
    return;
  }

  uint16_t budget = sizeof(chunk_) * 4;
  while (stream->available() > 0 && budget > 0) {
    const int available = stream->available();
    const size_t toRead = available > static_cast<int>(sizeof(chunk_)) ? sizeof(chunk_) : static_cast<size_t>(available);
    const int readBytes = stream->read(chunk_, toRead);
    if (readBytes <= 0) {
      break;
    }
    if (downloadedBytes_ + static_cast<uint32_t>(readBytes) > FlightSiteCatalogConfig::kMaximumCatalogBytes) {
      fail("CATALOGO EXCEDE LIMITE");
      return;
    }
    const size_t written = output_.write(chunk_, static_cast<size_t>(readBytes));
    if (written != static_cast<size_t>(readBytes)) {
      fail("FALHA AO GRAVAR CATALOGO");
      return;
    }
    downloadedBytes_ += static_cast<uint32_t>(readBytes);
    lastDataMs_ = millis();
    budget = budget > static_cast<uint16_t>(readBytes) ? budget - static_cast<uint16_t>(readBytes) : 0;
    if (contentLength_ > 0) {
      const uint32_t percent = downloadedBytes_ * 100UL / static_cast<uint32_t>(contentLength_);
      progressPercent_ = percent > 100UL ? 100 : static_cast<uint8_t>(percent);
    }
  }

  if (contentLength_ > 0 && downloadedBytes_ >= static_cast<uint32_t>(contentLength_)) {
    finishDownload();
    return;
  }
  if (!http_.connected() && stream->available() == 0) {
    if (contentLength_ <= 0 && downloadedBytes_ > 0) {
      finishDownload();
    } else {
      fail("DOWNLOAD DO CATALOGO INCOMPLETO");
    }
    return;
  }
  if (millis() - lastDataMs_ > FlightSiteCatalogConfig::kNoDataTimeoutMs) {
    fail("TIMEOUT DO CATALOGO");
  }
}

void FlightSiteCatalogUpdater::cancel() {
  if (!busy()) {
    return;
  }
  closeTransfer();
  if (filesystem_ && filesystem_->exists(kTempPath)) {
    filesystem_->remove(kTempPath);
  }
  state_ = FlightSiteCatalogUpdateState::Canceled;
  progressPercent_ = 0;
  setStatus("ATUALIZACAO CANCELADA");
}

void FlightSiteCatalogUpdater::resetStatus() {
  closeTransfer();
  state_ = FlightSiteCatalogUpdateState::Idle;
  progressPercent_ = 0;
  downloadedBytes_ = 0;
  contentLength_ = -1;
  waitStartedMs_ = 0;
  lastDataMs_ = 0;
  setStatus("CATALOGO PRONTO");
}

bool FlightSiteCatalogUpdater::isConfigured() const {
  return FlightSiteCatalogConfig::kCatalogUrl && FlightSiteCatalogConfig::kCatalogUrl[0] != '\0';
}

bool FlightSiteCatalogUpdater::startDownload() {
  if (!filesystem_ || !ensureDirectory()) {
    fail("LITTLEFS NAO PREPARADO");
    return false;
  }
  if (filesystem_->exists(kTempPath)) {
    filesystem_->remove(kTempPath);
  }
  output_ = filesystem_->open(kTempPath, FILE_WRITE);
  if (!output_) {
    fail("NAO ABRIU ARQUIVO TEMPORARIO");
    return false;
  }

  const char* url = FlightSiteCatalogConfig::kCatalogUrl;
  if (startsWithHttps(url)) {
    if (FlightSiteCatalogConfig::kAllowInsecureTls) {
      secureClient_.setInsecure();
    }
    if (!http_.begin(secureClient_, url)) {
      fail("FALHA URL HTTPS");
      return false;
    }
  } else if (!http_.begin(plainClient_, url)) {
    fail("FALHA URL HTTP");
    return false;
  }

  http_.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http_.setTimeout(FlightSiteCatalogConfig::kHttpTimeoutMs);
  const int code = http_.GET();
  if (code != HTTP_CODE_OK) {
    char error[40];
    snprintf(error, sizeof(error), "CATALOGO HTTP %d", code);
    fail(error);
    return false;
  }

  contentLength_ = http_.getSize();
  if (contentLength_ > static_cast<int32_t>(FlightSiteCatalogConfig::kMaximumCatalogBytes)) {
    fail("CATALOGO EXCEDE LIMITE");
    return false;
  }
  downloadedBytes_ = 0;
  progressPercent_ = 0;
  lastDataMs_ = millis();
  state_ = FlightSiteCatalogUpdateState::Downloading;
  setStatus("BAIXANDO ATUALIZACAO");
  Serial.printf("Rampas: baixando %s\n", url);
  return true;
}

void FlightSiteCatalogUpdater::finishDownload() {
  output_.flush();
  closeTransfer();
  state_ = FlightSiteCatalogUpdateState::Validating;
  setStatus("VALIDANDO CATALOGO");

  uint16_t siteCount = 0;
  uint32_t catalogVersion = 0;
  char updatedAt[20] = {};
  if (!filesystem_ ||
      !FlightSiteCatalog::validateFile(*filesystem_, kTempPath, &siteCount, &catalogVersion, updatedAt, sizeof(updatedAt))) {
    fail("ARQUIVO DE RAMPAS INVALIDO");
    return;
  }

  const char* finalPath = FlightSiteCatalog::catalogPath();
  if (filesystem_->exists(kBackupPath)) {
    filesystem_->remove(kBackupPath);
  }
  const bool hadCurrent = filesystem_->exists(finalPath);
  if (hadCurrent && !filesystem_->rename(finalPath, kBackupPath)) {
    fail("NAO CRIOU BACKUP DO CATALOGO");
    return;
  }
  if (!filesystem_->rename(kTempPath, finalPath)) {
    if (hadCurrent && filesystem_->exists(kBackupPath)) {
      filesystem_->rename(kBackupPath, finalPath);
    }
    fail("NAO INSTALOU NOVO CATALOGO");
    return;
  }
  if (!FlightSiteCatalog::reload()) {
    filesystem_->remove(finalPath);
    if (hadCurrent && filesystem_->exists(kBackupPath)) {
      filesystem_->rename(kBackupPath, finalPath);
    }
    FlightSiteCatalog::reload();
    fail("NOVO CATALOGO NAO ABRIU");
    return;
  }
  if (filesystem_->exists(kBackupPath)) {
    filesystem_->remove(kBackupPath);
  }

  progressPercent_ = 100;
  state_ = FlightSiteCatalogUpdateState::Success;
  char status[72];
  snprintf(status,
           sizeof(status),
           "LISTA ATUALIZADA - %u RAMPAS",
           static_cast<unsigned>(siteCount));
  setStatus(status);
  Serial.printf("Rampas: catalogo V%lu instalado, %u rampas, data %s.\n",
                static_cast<unsigned long>(catalogVersion),
                static_cast<unsigned>(siteCount),
                updatedAt);
}

void FlightSiteCatalogUpdater::fail(const char* text) {
  closeTransfer();
  if (filesystem_ && filesystem_->exists(kTempPath)) {
    filesystem_->remove(kTempPath);
  }
  state_ = FlightSiteCatalogUpdateState::Failed;
  progressPercent_ = 0;
  setStatus(text);
  Serial.printf("Rampas: %s\n", statusText_);
}

void FlightSiteCatalogUpdater::setStatus(const char* text) {
  snprintf(statusText_, sizeof(statusText_), "%s", text ? text : "");
}

void FlightSiteCatalogUpdater::closeTransfer() {
  if (output_) {
    output_.close();
  }
  http_.end();
}

bool FlightSiteCatalogUpdater::ensureDirectory() {
  if (!filesystem_) {
    return false;
  }
  if (filesystem_->exists("/weather")) {
    File directory = filesystem_->open("/weather", FILE_READ);
    const bool valid = directory && directory.isDirectory();
    directory.close();
    return valid;
  }
  return filesystem_->mkdir("/weather");
}
