#pragma once

#include <Arduino.h>
#include <touch/TouchDrvGT911.hpp>

class TouchManager {
 public:
  bool begin();
  void sleep();
  bool update();
  void settleAfterAction();
  bool wasTouched();
  bool isPressed() const;
  bool isReady() const { return ready; }
  int16_t getX();
  int16_t getY();

 private:
  TouchDrvGT911 touch;
  void releaseI2CBus();
  void wakeController();
  bool detectAddress();
  bool configureTouch();
  bool ping();
  bool recover(const char* reason);

  int16_t touchX = 0;
  int16_t touchY = 0;
  uint8_t touchAddress = 0;
  bool touched = false;
  bool ready = false;
  bool pressed = false;
  bool suppressedUntilRelease = false;
  uint32_t nextRead = 0;
  uint32_t nextHealthCheckMs = 0;
  uint32_t lastRecoveryMs = 0;
  uint32_t pressStartedMs = 0;
  uint32_t lastPointMs = 0;
  uint32_t suppressedSinceMs = 0;
};
