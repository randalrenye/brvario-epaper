#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class I2CBusLock {
 public:
  static bool begin();
  static bool take(TickType_t timeoutTicks);
  static void give();
  static uint32_t timeoutCount();

 private:
  static SemaphoreHandle_t mutex_;
  static volatile uint32_t timeoutCount_;
};

class I2CBusGuard {
 public:
  explicit I2CBusGuard(TickType_t timeoutTicks = pdMS_TO_TICKS(50))
      : locked_(I2CBusLock::take(timeoutTicks)) {}

  ~I2CBusGuard() {
    if (locked_) {
      I2CBusLock::give();
    }
  }

  I2CBusGuard(const I2CBusGuard&) = delete;
  I2CBusGuard& operator=(const I2CBusGuard&) = delete;

  bool locked() const { return locked_; }

 private:
  bool locked_ = false;
};
