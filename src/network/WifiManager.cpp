#include "network/WifiManager.h"

#include <Preferences.h>
#include <WiFi.h>

namespace {

static constexpr char kWifiPrefsNamespace[] = "wifi";
static constexpr char kWifiSsidKey[] = "ssid";
static constexpr char kWifiPasswordKey[] = "pass";
static constexpr char kWifiSavedCountKey[] = "count";
static constexpr uint32_t kConnectTimeoutMs = 10000UL;
static constexpr uint32_t kEarlyFailureGraceMs = 1500UL;
static constexpr uint32_t kScanTimeoutMs = 15000UL;

}  // namespace

bool WifiManager::begin() {
  loadCredentials();
  enabled_ = false;
  powerOffRadio();
  return true;
}

void WifiManager::update() {
  if (!enabled_) {
    state_ = WifiConnectionState::Off;
    return;
  }

  if (scanActive_ || state_ == WifiConnectionState::Scanning) {
    const int result = WiFi.scanComplete();
    if (result == WIFI_SCAN_RUNNING) {
      if (scanStartedMs_ != 0 && millis() - scanStartedMs_ > kScanTimeoutMs) {
        Serial.println("WiFi: busca de redes excedeu o tempo limite.");
        cancelScan();
      }
      return;
    }

    finishScan(result);
    return;
  }

  if (state_ == WifiConnectionState::Connecting) {
    const wl_status_t wifiStatus = WiFi.status();
    if (wifiStatus == WL_CONNECTED) {
      state_ = WifiConnectionState::Connected;
      savedSsid_ = pendingSsid_;
      savedPassword_ = pendingPassword_;
      const bool promoteSuccessfulSavedNetwork = autoConnectActive_ && autoConnectIndex_ > 0;
      if (pendingSave_ || promoteSuccessfulSavedNetwork) {
        addOrUpdateCredential(savedSsid_, savedPassword_);
      }
      pendingSave_ = false;
      stopAutoConnect();
      Serial.printf("WiFi conectado: %s IP=%s\n", savedSsid_.c_str(), WiFi.localIP().toString().c_str());
      return;
    }

    const uint32_t elapsedMs = millis() - connectStartedMs_;
    const bool failedEarly =
        elapsedMs >= kEarlyFailureGraceMs && (wifiStatus == WL_NO_SSID_AVAIL || wifiStatus == WL_CONNECT_FAILED);
    if (failedEarly || elapsedMs > kConnectTimeoutMs) {
      finishConnectionFailure(failedEarly ? "rede indisponivel ou senha rejeitada" : "tempo limite");
    }
    return;
  }

  if (state_ == WifiConnectionState::Connected && WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi: conexao perdida; tentando novamente as redes salvas.");
    if (!connectSaved()) {
      state_ = WifiConnectionState::Failed;
    }
  }
}

void WifiManager::end() {
  powerOffRadio();
}

void WifiManager::setEnabled(bool enabled) {
  if (enabled == enabled_) {
    if (enabled_ && state_ == WifiConnectionState::Off) {
      powerOnRadio();
    }
    return;
  }

  enabled_ = enabled;
  if (!enabled_) {
    powerOffRadio();
    return;
  }

  powerOnRadio();
  if (savedCredentialCount_ > 0) {
    connectSaved();
  }
}

void WifiManager::enableRuntime(bool connectSavedNetwork) {
  enabled_ = true;
  if (state_ == WifiConnectionState::Off) {
    powerOnRadio();
  }
  if (connectSavedNetwork && savedCredentialCount_ > 0 && !isConnected() && state_ != WifiConnectionState::Connecting) {
    connectSaved();
  }
}

void WifiManager::disableRuntime() {
  powerOffRadio();
  enabled_ = false;
}

int WifiManager::scanNetworks() {
  if (!enabled_) {
    clearNetworks();
    state_ = WifiConnectionState::Off;
    return 0;
  }

  powerOnRadio();
  stopAutoConnect();
  if (scanActive_ || state_ == WifiConnectionState::Scanning) {
    return WIFI_SCAN_RUNNING;
  }

  state_ = WifiConnectionState::Scanning;
  clearNetworks();
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(false);
  }

  const int started = WiFi.scanNetworks(true, true);
  if (started == WIFI_SCAN_FAILED) {
    Serial.println("WiFi: falha ao iniciar busca de redes.");
    state_ = isConnected() ? WifiConnectionState::Connected : WifiConnectionState::Idle;
    return 0;
  }
  if (started >= 0) {
    finishScan(started);
    return started;
  }

  scanActive_ = true;
  scanStartedMs_ = millis();
  Serial.println("WiFi: busca de redes iniciada em segundo plano.");
  return started;
}

bool WifiManager::connectTo(const String& ssid, const String& password) {
  stopAutoConnect();
  return startConnection(ssid, password, true);
}

bool WifiManager::startConnection(const String& ssid, const String& password, bool saveOnSuccess) {
  if (!enabled_) {
    state_ = WifiConnectionState::Off;
    return false;
  }

  if (ssid.length() == 0) {
    state_ = WifiConnectionState::Failed;
    return false;
  }

  pendingSsid_ = ssid;
  pendingPassword_ = password;
  pendingSave_ = saveOnSuccess;
  powerOnRadio();
  cancelScan();
  WiFi.disconnect(false);
  WiFi.begin(ssid.c_str(), password.c_str());
  connectStartedMs_ = millis();
  state_ = WifiConnectionState::Connecting;
  Serial.printf("WiFi: conectando em %s\n", ssid.c_str());
  return true;
}

bool WifiManager::connectSaved() {
  if (!enabled_ || savedCredentialCount_ == 0) {
    stopAutoConnect();
    state_ = enabled_ ? WifiConnectionState::Idle : WifiConnectionState::Off;
    return false;
  }

  if (isConnected()) {
    return true;
  }
  if (state_ == WifiConnectionState::Connecting && autoConnectActive_) {
    return true;
  }

  autoConnectActive_ = true;
  autoConnectIndex_ = 0;
  return startSavedCredential(autoConnectIndex_);
}

void WifiManager::disconnect() {
  stopAutoConnect();
  cancelScan();
  WiFi.disconnect(false);
  state_ = enabled_ ? WifiConnectionState::Idle : WifiConnectionState::Off;
}

bool WifiManager::clearCredentials() {
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    Serial.println("WiFi: falha ao abrir NVS para limpar credenciais.");
    return false;
  }
  prefs.clear();
  prefs.end();
  savedSsid_ = "";
  savedPassword_ = "";
  pendingSsid_ = "";
  pendingPassword_ = "";
  for (uint8_t i = 0; i < kMaxSavedCredentials; ++i) {
    savedCredentials_[i].ssid = "";
    savedCredentials_[i].password = "";
  }
  savedCredentialCount_ = 0;
  pendingSave_ = false;
  stopAutoConnect();
  cancelScan();
  if (enabled_) {
    WiFi.disconnect(false);
    state_ = WifiConnectionState::Idle;
  }
  Serial.println("WiFi: credenciais apagadas.");
  return true;
}

bool WifiManager::isConnected() const {
  return enabled_ && WiFi.status() == WL_CONNECTED;
}

const char* WifiManager::statusText() const {
  switch (state_) {
    case WifiConnectionState::Off:
      return "DESLIGADO";
    case WifiConnectionState::Scanning:
      return "BUSCANDO REDES";
    case WifiConnectionState::Connecting:
      return "CONECTANDO";
    case WifiConnectionState::Connected:
      return "CONECTADO";
    case WifiConnectionState::Failed:
      return "FALHA NA CONEXAO";
    case WifiConnectionState::Idle:
    default:
      return "NAO CONECTADO";
  }
}

String WifiManager::currentSsid() const {
  if (isConnected()) {
    return WiFi.SSID();
  }
  return pendingSsid_.length() > 0 ? pendingSsid_ : savedSsid_;
}

String WifiManager::ipText() const {
  if (!isConnected()) {
    return "";
  }
  return WiFi.localIP().toString();
}

String WifiManager::savedCredentialSsid(uint8_t index) const {
  return index < savedCredentialCount_ ? savedCredentials_[index].ssid : String();
}

String WifiManager::savedPasswordFor(const String& ssid) const {
  const int8_t index = savedCredentialIndexOf(ssid);
  return index >= 0 ? savedCredentials_[index].password : String();
}

bool WifiManager::hasSavedCredential(const String& ssid) const {
  return savedCredentialIndexOf(ssid) >= 0;
}

String WifiManager::networkSsid(uint8_t index) const {
  return index < networkCount_ ? networks_[index] : String();
}

int32_t WifiManager::networkRssi(uint8_t index) const {
  return index < networkCount_ ? rssi_[index] : 0;
}

bool WifiManager::loadCredentials() {
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  savedSsid_ = prefs.getString(kWifiSsidKey, "");
  savedPassword_ = prefs.getString(kWifiPasswordKey, "");
  savedCredentialCount_ = 0;

  const uint8_t storedCount = prefs.getUChar(kWifiSavedCountKey, 0);
  const uint8_t count = storedCount < kMaxSavedCredentials ? storedCount : kMaxSavedCredentials;
  for (uint8_t i = 0; i < count; ++i) {
    char ssidKey[8];
    char passKey[8];
    snprintf(ssidKey, sizeof(ssidKey), "s%u", static_cast<unsigned>(i));
    snprintf(passKey, sizeof(passKey), "p%u", static_cast<unsigned>(i));
    const String ssid = prefs.getString(ssidKey, "");
    if (ssid.length() == 0) {
      continue;
    }
    savedCredentials_[savedCredentialCount_].ssid = ssid;
    savedCredentials_[savedCredentialCount_].password = prefs.getString(passKey, "");
    ++savedCredentialCount_;
  }
  prefs.end();

  if (savedCredentialCount_ == 0 && savedSsid_.length() > 0) {
    savedCredentials_[0].ssid = savedSsid_;
    savedCredentials_[0].password = savedPassword_;
    savedCredentialCount_ = 1;
    saveCredentialList();
  } else if (savedCredentialCount_ > 0) {
    savedSsid_ = savedCredentials_[0].ssid;
    savedPassword_ = savedCredentials_[0].password;
  }
  return savedCredentialCount_ > 0;
}

bool WifiManager::saveCredentials(const String& ssid, const String& password) {
  return addOrUpdateCredential(ssid, password);
}

bool WifiManager::saveCredentialList() {
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    Serial.println("WiFi: falha ao abrir NVS.");
    return false;
  }
  prefs.putUChar(kWifiSavedCountKey, savedCredentialCount_);
  for (uint8_t i = 0; i < kMaxSavedCredentials; ++i) {
    char ssidKey[8];
    char passKey[8];
    snprintf(ssidKey, sizeof(ssidKey), "s%u", static_cast<unsigned>(i));
    snprintf(passKey, sizeof(passKey), "p%u", static_cast<unsigned>(i));
    if (i < savedCredentialCount_) {
      prefs.putString(ssidKey, savedCredentials_[i].ssid);
      prefs.putString(passKey, savedCredentials_[i].password);
    } else {
      prefs.remove(ssidKey);
      prefs.remove(passKey);
    }
  }
  if (savedCredentialCount_ > 0) {
    prefs.putString(kWifiSsidKey, savedCredentials_[0].ssid);
    prefs.putString(kWifiPasswordKey, savedCredentials_[0].password);
  } else {
    prefs.remove(kWifiSsidKey);
    prefs.remove(kWifiPasswordKey);
  }
  prefs.end();
  Serial.printf("WiFi: %u credenciais salvas.\n", static_cast<unsigned>(savedCredentialCount_));
  return true;
}

bool WifiManager::addOrUpdateCredential(const String& ssid, const String& password) {
  if (ssid.length() == 0) {
    return false;
  }

  const int8_t existing = savedCredentialIndexOf(ssid);
  SavedCredential updated = {ssid, password};
  if (existing >= 0) {
    updated.password = password;
    for (int8_t i = existing; i > 0; --i) {
      savedCredentials_[i] = savedCredentials_[i - 1];
    }
    savedCredentials_[0] = updated;
  } else {
    const uint8_t limit = savedCredentialCount_ < kMaxSavedCredentials ? savedCredentialCount_ : kMaxSavedCredentials - 1;
    for (int8_t i = static_cast<int8_t>(limit); i > 0; --i) {
      savedCredentials_[i] = savedCredentials_[i - 1];
    }
    savedCredentials_[0] = updated;
    if (savedCredentialCount_ < kMaxSavedCredentials) {
      ++savedCredentialCount_;
    }
  }

  savedSsid_ = savedCredentials_[0].ssid;
  savedPassword_ = savedCredentials_[0].password;
  return saveCredentialList();
}

bool WifiManager::startSavedCredential(uint8_t index) {
  if (!enabled_ || index >= savedCredentialCount_) {
    return false;
  }

  const SavedCredential& credential = savedCredentials_[index];
  if (credential.ssid.length() == 0) {
    return false;
  }

  autoConnectIndex_ = index;
  Serial.printf("WiFi: tentativa automatica %u/%u em %s\n",
                static_cast<unsigned>(index + 1),
                static_cast<unsigned>(savedCredentialCount_),
                credential.ssid.c_str());
  return startConnection(credential.ssid, credential.password, false);
}

bool WifiManager::tryNextSavedCredential() {
  if (!autoConnectActive_) {
    return false;
  }

  for (uint8_t next = static_cast<uint8_t>(autoConnectIndex_ + 1); next < savedCredentialCount_; ++next) {
    if (startSavedCredential(next)) {
      return true;
    }
  }
  return false;
}

void WifiManager::finishConnectionFailure(const char* reason) {
  Serial.printf("WiFi: falha em %s (%s).\n", pendingSsid_.c_str(), reason ? reason : "erro");
  WiFi.disconnect(false);
  pendingSave_ = false;

  if (tryNextSavedCredential()) {
    return;
  }

  stopAutoConnect();
  pendingSsid_ = "";
  pendingPassword_ = "";
  state_ = WifiConnectionState::Failed;
  Serial.println("WiFi: nenhuma das redes salvas conectou.");
}

void WifiManager::stopAutoConnect() {
  autoConnectActive_ = false;
  autoConnectIndex_ = 0;
}

int8_t WifiManager::savedCredentialIndexOf(const String& ssid) const {
  for (uint8_t i = 0; i < savedCredentialCount_; ++i) {
    if (savedCredentials_[i].ssid == ssid) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

void WifiManager::powerOnRadio() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  state_ = isConnected() ? WifiConnectionState::Connected : WifiConnectionState::Idle;
}

void WifiManager::powerOffRadio() {
  stopAutoConnect();
  cancelScan();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  clearNetworks();
  pendingSave_ = false;
  state_ = WifiConnectionState::Off;
}

void WifiManager::clearNetworks() {
  for (uint8_t i = 0; i < kMaxNetworks; ++i) {
    networks_[i] = "";
    rssi_[i] = 0;
  }
  networkCount_ = 0;
}

void WifiManager::cancelScan() {
  if (scanActive_ || state_ == WifiConnectionState::Scanning) {
    WiFi.scanDelete();
  }
  scanActive_ = false;
  scanStartedMs_ = 0;
  if (state_ == WifiConnectionState::Scanning) {
    state_ = enabled_ ? (isConnected() ? WifiConnectionState::Connected : WifiConnectionState::Idle) : WifiConnectionState::Off;
  }
}

void WifiManager::finishScan(int found) {
  clearNetworks();
  if (found > 0) {
    const uint8_t count = found < kMaxNetworks ? static_cast<uint8_t>(found) : kMaxNetworks;
    for (uint8_t i = 0; i < count; ++i) {
      networks_[i] = WiFi.SSID(i);
      rssi_[i] = WiFi.RSSI(i);
    }
    networkCount_ = count;
    Serial.printf("WiFi: busca concluida, %d redes encontradas.\n", found);
  } else {
    Serial.printf("WiFi: busca concluida sem redes (%d).\n", found);
  }
  WiFi.scanDelete();
  scanActive_ = false;
  scanStartedMs_ = 0;
  state_ = isConnected() ? WifiConnectionState::Connected : WifiConnectionState::Idle;
}
