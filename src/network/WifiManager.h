#pragma once

#include <Arduino.h>
#include <stdint.h>

enum class WifiConnectionState : uint8_t {
  Off = 0,
  Idle,
  Scanning,
  Connecting,
  Connected,
  Failed,
};

class WifiManager {
 public:
  static constexpr uint8_t kMaxNetworks = 3;
  static constexpr uint8_t kMaxSavedCredentials = 10;

  bool begin();
  void update();
  void end();
  void setEnabled(bool enabled);
  void enableRuntime(bool connectSavedNetwork = true);
  void disableRuntime();

  int scanNetworks();
  bool connectTo(const String& ssid, const String& password);
  bool connectSaved();
  void disconnect();
  bool clearCredentials();

  bool isEnabled() const { return enabled_; }
  bool isConnected() const;
  WifiConnectionState state() const { return state_; }
  const char* statusText() const;
  String currentSsid() const;
  String ipText() const;
  String savedSsid() const { return savedSsid_; }
  uint8_t savedCredentialCount() const { return savedCredentialCount_; }
  String savedCredentialSsid(uint8_t index) const;
  String savedPasswordFor(const String& ssid) const;
  bool hasSavedCredential(const String& ssid) const;

  uint8_t networkCount() const { return networkCount_; }
  String networkSsid(uint8_t index) const;
  int32_t networkRssi(uint8_t index) const;

 private:
  struct SavedCredential {
    String ssid;
    String password;
  };

  WifiConnectionState state_ = WifiConnectionState::Off;
  String savedSsid_;
  String savedPassword_;
  String pendingSsid_;
  String pendingPassword_;
  SavedCredential savedCredentials_[kMaxSavedCredentials];
  String networks_[kMaxNetworks];
  int32_t rssi_[kMaxNetworks] = {};
  uint8_t savedCredentialCount_ = 0;
  uint8_t networkCount_ = 0;
  uint32_t connectStartedMs_ = 0;
  uint32_t scanStartedMs_ = 0;
  bool pendingSave_ = false;
  bool enabled_ = false;
  bool scanActive_ = false;
  bool autoConnectActive_ = false;
  uint8_t autoConnectIndex_ = 0;

  bool loadCredentials();
  bool saveCredentials(const String& ssid, const String& password);
  bool saveCredentialList();
  bool addOrUpdateCredential(const String& ssid, const String& password);
  bool startConnection(const String& ssid, const String& password, bool saveOnSuccess);
  bool startSavedCredential(uint8_t index);
  bool tryNextSavedCredential();
  void finishConnectionFailure(const char* reason);
  void stopAutoConnect();
  int8_t savedCredentialIndexOf(const String& ssid) const;
  void powerOnRadio();
  void powerOffRadio();
  void clearNetworks();
  void cancelScan();
  void finishScan(int found);
};
