#pragma once

#include <Arduino.h>
#include <stdint.h>

enum class FirmwareUpdateState : uint8_t {
  Idle = 0,
  NoWifi,
  NoUrl,
  Downloading,
  Success,
  Failed,
};

class FirmwareUpdater {
 public:
  bool beginUpdate();
  void resetStatus();

  FirmwareUpdateState state() const { return state_; }
  const char* statusText() const;
  const String& errorText() const { return errorText_; }
  uint8_t progressPercent() const { return progressPercent_; }
  bool isConfigured() const;
  const char* firmwareUrl() const;

 private:
  FirmwareUpdateState state_ = FirmwareUpdateState::Idle;
  String errorText_;
  uint8_t progressPercent_ = 0;

  void fail(const String& message);
};
