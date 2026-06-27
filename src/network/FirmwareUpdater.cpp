#include "network/FirmwareUpdater.h"

#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "config/FirmwareUpdateConfig.h"

bool FirmwareUpdater::beginUpdate() {
  errorText_ = "";
  progressPercent_ = 0;

  if (WiFi.status() != WL_CONNECTED) {
    fail("WiFi nao conectado");
    state_ = FirmwareUpdateState::NoWifi;
    return false;
  }

  const char* url = FirmwareUpdateConfig::kFirmwareBinUrl;
  if (!url || url[0] == '\0') {
    fail("Link .bin nao configurado");
    state_ = FirmwareUpdateState::NoUrl;
    return false;
  }

  state_ = FirmwareUpdateState::Downloading;
  Serial.printf("OTA: baixando firmware de %s\n", url);

  WiFiClientSecure client;
  if (FirmwareUpdateConfig::kAllowInsecureTls) {
    client.setInsecure();
  } else if (FirmwareUpdateConfig::kRootCaPem && FirmwareUpdateConfig::kRootCaPem[0] != '\0') {
    client.setCACert(FirmwareUpdateConfig::kRootCaPem);
  } else {
    fail("Certificado TLS nao configurado");
    return false;
  }

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(20000);
  if (!http.begin(client, url)) {
    fail("Falha ao abrir URL");
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    fail(String("HTTP ") + String(code));
    http.end();
    return false;
  }

  const int contentLength = http.getSize();
  if (!Update.begin(contentLength > 0 ? static_cast<size_t>(contentLength) : UPDATE_SIZE_UNKNOWN)) {
    fail(String("OTA begin: ") + Update.errorString());
    http.end();
    return false;
  }

  const size_t written = Update.writeStream(http.getStream());
  if (contentLength > 0 && written != static_cast<size_t>(contentLength)) {
    fail(String("Download incompleto ") + String(written) + "/" + String(contentLength));
    Update.abort();
    http.end();
    return false;
  }

  if (!Update.end(true)) {
    fail(String("OTA end: ") + Update.errorString());
    http.end();
    return false;
  }

  if (!Update.isFinished()) {
    fail("OTA nao finalizado");
    http.end();
    return false;
  }

  http.end();
  progressPercent_ = 100;
  state_ = FirmwareUpdateState::Success;
  Serial.println("OTA: firmware atualizado. Reiniciando...");
  return true;
}

void FirmwareUpdater::resetStatus() {
  state_ = FirmwareUpdateState::Idle;
  errorText_ = "";
  progressPercent_ = 0;
}

const char* FirmwareUpdater::statusText() const {
  switch (state_) {
    case FirmwareUpdateState::NoWifi:
      return "WIFI NECESSARIO";
    case FirmwareUpdateState::NoUrl:
      return "LINK NAO CONFIGURADO";
    case FirmwareUpdateState::Downloading:
      return "BAIXANDO";
    case FirmwareUpdateState::Success:
      return "ATUALIZADO";
    case FirmwareUpdateState::Failed:
      return "FALHA";
    case FirmwareUpdateState::Idle:
    default:
      return "PRONTO";
  }
}

bool FirmwareUpdater::isConfigured() const {
  return FirmwareUpdateConfig::kFirmwareBinUrl && FirmwareUpdateConfig::kFirmwareBinUrl[0] != '\0';
}

const char* FirmwareUpdater::firmwareUrl() const {
  return FirmwareUpdateConfig::kFirmwareBinUrl;
}

void FirmwareUpdater::fail(const String& message) {
  state_ = FirmwareUpdateState::Failed;
  errorText_ = message;
  Serial.printf("OTA: %s\n", errorText_.c_str());
}
