#include "system/I2CBusLock.h"

SemaphoreHandle_t I2CBusLock::mutex_ = nullptr;
volatile uint32_t I2CBusLock::timeoutCount_ = 0;

bool I2CBusLock::begin() {
  if (mutex_ != nullptr) {
    return true;
  }

  // Called during setup before the barometer task starts.
  mutex_ = xSemaphoreCreateRecursiveMutex();
  return mutex_ != nullptr;
}

bool I2CBusLock::take(TickType_t timeoutTicks) {
  if (!begin()) {
    __atomic_add_fetch(&timeoutCount_, 1UL, __ATOMIC_RELAXED);
    return false;
  }
  if (xSemaphoreTakeRecursive(mutex_, timeoutTicks) == pdTRUE) {
    return true;
  }
  __atomic_add_fetch(&timeoutCount_, 1UL, __ATOMIC_RELAXED);
  return false;
}

void I2CBusLock::give() {
  if (mutex_ != nullptr) {
    xSemaphoreGiveRecursive(mutex_);
  }
}

uint32_t I2CBusLock::timeoutCount() {
  return __atomic_load_n(&timeoutCount_, __ATOMIC_RELAXED);
}
