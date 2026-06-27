#include "TouchManager.h"

#include <Wire.h>

#include "epd_driver.h"
#include "system/I2CBusLock.h"
#include "utilities.h"

namespace {

static constexpr uint32_t kTouchPollIntervalMs = 4;
static constexpr uint32_t kTouchReleaseGraceMs = 12;
static constexpr uint32_t kTouchFastRetapMs = 55;
static constexpr int16_t kTouchFastRetapDistancePx = 72;
static constexpr uint32_t kTouchHealthIntervalMs = 5000;
static constexpr uint32_t kTouchRecoveryIntervalMs = 3000;
static constexpr uint32_t kTouchStuckPressMs = 2500;
static constexpr uint32_t kTouchWireTimeoutMs = 50;
static constexpr uint32_t kTouchI2cClockHz = 400000;

}  // namespace

bool TouchManager::begin() {
  I2CBusGuard bus(pdMS_TO_TICKS(250));
  if (!bus.locked()) {
    Serial.println("Touch GT911: I2C ocupado durante inicializacao.");
    return false;
  }

  ready = false;
  touched = false;
  pressed = false;
  suppressedUntilRelease = false;
  suppressedSinceMs = 0;

  releaseI2CBus();
  Wire.begin(BOARD_SDA, BOARD_SCL);
  Wire.setClock(kTouchI2cClockHz);
  Wire.setTimeOut(kTouchWireTimeoutMs);
  wakeController();

  if (!detectAddress()) {
    Serial.println("Touch GT911 nao encontrado");
    return false;
  }

  if (!configureTouch()) {
    return false;
  }

  ready = true;
  nextRead = millis() + kTouchPollIntervalMs;
  nextHealthCheckMs = millis() + kTouchHealthIntervalMs;
  lastRecoveryMs = millis();
  pressStartedMs = 0;
  lastPointMs = 0;
  Serial.printf("Touch GT911 iniciado no endereco 0x%02X\n", touchAddress);
  return true;
}

void TouchManager::sleep() {
  I2CBusGuard bus(pdMS_TO_TICKS(250));
  if (!bus.locked()) {
    ready = false;
    touched = false;
    pressed = false;
    return;
  }

  if (ready) {
    touch.sleep();
  }
  Wire.end();
  pinMode(BOARD_SDA, OPEN_DRAIN);
  pinMode(BOARD_SCL, OPEN_DRAIN);
  pinMode(TOUCH_INT, OPEN_DRAIN);
  ready = false;
  touched = false;
  pressed = false;
  suppressedUntilRelease = false;
  suppressedSinceMs = 0;
  lastPointMs = 0;
}

bool TouchManager::update() {
  touched = false;
  const uint32_t now = millis();

  if (!ready) {
    if (lastRecoveryMs == 0 || now - lastRecoveryMs >= kTouchRecoveryIntervalMs) {
      lastRecoveryMs = now;
      begin();
    }
    return false;
  }

  if (now < nextRead) {
    return false;
  }
  nextRead = now + kTouchPollIntervalMs;

  if (now >= nextHealthCheckMs) {
    nextHealthCheckMs = now + kTouchHealthIntervalMs;
    if (!ping()) {
      return recover("sem resposta I2C");
    }
  }

  bool hasPoints = false;
  uint16_t pointX = 0;
  uint16_t pointY = 0;
  {
    I2CBusGuard bus(pdMS_TO_TICKS(30));
    if (!bus.locked()) {
      return false;
    }
    const TouchPoints& points = touch.getTouchPoints();
    hasPoints = points.hasPoints();
    if (hasPoints) {
      const TouchPoint& point = points.getPoint(0);
      pointX = point.x;
      pointY = point.y;
    }
  }

  if (!hasPoints) {
    if (pressed && lastPointMs != 0 && now - lastPointMs > kTouchReleaseGraceMs) {
      pressed = false;
      suppressedUntilRelease = false;
      suppressedSinceMs = 0;
      pressStartedMs = 0;
    }
    return false;
  }

  if (pointX >= EPD_WIDTH || pointY >= EPD_HEIGHT) {
    return recover("coordenada invalida");
  }

  const int16_t newX = static_cast<int16_t>(pointX);
  const int16_t newY = static_cast<int16_t>(pointY);

  if (pressed && pressStartedMs != 0 && now - pressStartedMs >= kTouchStuckPressMs) {
    suppressedUntilRelease = true;
    suppressedSinceMs = suppressedSinceMs == 0 ? now : suppressedSinceMs;
    return recover("toque preso");
  }

  if (suppressedUntilRelease) {
    const int32_t dx = static_cast<int32_t>(newX) - touchX;
    const int32_t dy = static_cast<int32_t>(newY) - touchY;
    const bool movedToNewTarget = (abs(dx) >= kTouchFastRetapDistancePx) || (abs(dy) >= kTouchFastRetapDistancePx);
    if (suppressedSinceMs != 0 && now - suppressedSinceMs >= kTouchFastRetapMs && movedToNewTarget) {
      touchX = newX;
      touchY = newY;
      lastPointMs = now;
      pressed = true;
      suppressedUntilRelease = false;
      suppressedSinceMs = 0;
      pressStartedMs = now;
      touched = true;
      return true;
    }
    touchX = newX;
    touchY = newY;
    lastPointMs = now;
    return false;
  }

  if (pressed) {
    touchX = newX;
    touchY = newY;
    lastPointMs = now;
    return false;
  }

  touchX = newX;
  touchY = newY;
  lastPointMs = now;
  pressed = true;
  pressStartedMs = now;
  touched = true;
  return true;
}

void TouchManager::settleAfterAction() {
  if (!ready) {
    return;
  }

  touched = false;
  nextRead = millis();

  bool hasPoints = false;
  uint16_t pointX = 0;
  uint16_t pointY = 0;
  {
    I2CBusGuard bus(pdMS_TO_TICKS(30));
    if (!bus.locked()) {
      return;
    }
    const TouchPoints& points = touch.getTouchPoints();
    hasPoints = points.hasPoints();
    if (hasPoints) {
      const TouchPoint& point = points.getPoint(0);
      pointX = point.x;
      pointY = point.y;
    }
  }

  if (!hasPoints) {
    pressed = false;
    suppressedUntilRelease = false;
    suppressedSinceMs = 0;
    pressStartedMs = 0;
    lastPointMs = 0;
    return;
  }

  if (pointX >= EPD_WIDTH || pointY >= EPD_HEIGHT) {
    recover("coordenada invalida apos acao");
    return;
  }

  touchX = static_cast<int16_t>(pointX);
  touchY = static_cast<int16_t>(pointY);
  lastPointMs = millis();
  pressed = true;
  suppressedUntilRelease = true;
  suppressedSinceMs = lastPointMs;
  if (pressStartedMs == 0) {
    pressStartedMs = lastPointMs;
  }
}

bool TouchManager::wasTouched() {
  return touched;
}

bool TouchManager::isPressed() const {
  return pressed;
}

int16_t TouchManager::getX() {
  return touchX;
}

int16_t TouchManager::getY() {
  return touchY;
}

void TouchManager::releaseI2CBus() {
  Wire.end();

  pinMode(BOARD_SDA, INPUT_PULLUP);
  pinMode(BOARD_SCL, INPUT_PULLUP);
  delayMicroseconds(20);

  // If a device keeps SDA low after an aborted transfer, clock SCL manually
  // until it releases the bus, then generate a STOP condition.
  for (uint8_t i = 0; i < 18 && digitalRead(BOARD_SDA) == LOW; ++i) {
    pinMode(BOARD_SCL, OUTPUT_OPEN_DRAIN);
    digitalWrite(BOARD_SCL, LOW);
    delayMicroseconds(8);
    pinMode(BOARD_SCL, INPUT_PULLUP);
    delayMicroseconds(8);
  }

  pinMode(BOARD_SDA, OUTPUT_OPEN_DRAIN);
  digitalWrite(BOARD_SDA, LOW);
  delayMicroseconds(8);
  pinMode(BOARD_SCL, INPUT_PULLUP);
  delayMicroseconds(8);
  pinMode(BOARD_SDA, INPUT_PULLUP);
  delayMicroseconds(20);
}

void TouchManager::wakeController() {
  // GT911 keeps its own power during ESP32 reset. If it was asleep, a reset
  // button reboot is not enough; INT must be driven high before I2C probing.
  pinMode(TOUCH_INT, OUTPUT);
  digitalWrite(TOUCH_INT, HIGH);
  delay(12);
  pinMode(TOUCH_INT, INPUT_PULLUP);
  delay(40);
}

bool TouchManager::detectAddress() {
  touchAddress = 0;

  const uint8_t addresses[] = {0x14, 0x5D};
  for (uint8_t address : addresses) {
    for (uint8_t attempt = 0; attempt < 3; ++attempt) {
      Wire.beginTransmission(address);
      if (Wire.endTransmission() == 0) {
        touchAddress = address;
        return true;
      }
      delay(8);
    }
  }

  return false;
}

bool TouchManager::configureTouch() {
  wakeController();

  touch.setPins(-1, TOUCH_INT);
  if (!touch.begin(Wire, touchAddress, BOARD_SDA, BOARD_SCL)) {
    Serial.println("Falha ao inicializar Touch GT911");
    ready = false;
    return false;
  }

  touch.setMaxCoordinates(EPD_WIDTH, EPD_HEIGHT);
  touch.setSwapXY(true);
  touch.setMirrorXY(false, true);
  return true;
}

bool TouchManager::ping() {
  if (touchAddress == 0) {
    return false;
  }

  I2CBusGuard bus(pdMS_TO_TICKS(40));
  if (!bus.locked()) {
    // Another legitimate I2C transaction is not a GT911 failure.
    return true;
  }
  Wire.beginTransmission(touchAddress);
  return Wire.endTransmission() == 0;
}

bool TouchManager::recover(const char* reason) {
  I2CBusGuard bus(pdMS_TO_TICKS(300));
  if (!bus.locked()) {
    Serial.println("Touch GT911: recuperacao adiada, barramento I2C ocupado.");
    return false;
  }

  Serial.print("Touch GT911: recuperacao automatica");
  if (reason && reason[0] != '\0') {
    Serial.print(" - ");
    Serial.print(reason);
  }
  Serial.println();

  ready = false;
  touched = false;
  pressed = false;
  suppressedUntilRelease = false;
  suppressedSinceMs = 0;
  pressStartedMs = 0;
  lastPointMs = 0;
  lastRecoveryMs = millis();

  releaseI2CBus();
  delay(30);
  Wire.begin(BOARD_SDA, BOARD_SCL);
  Wire.setClock(kTouchI2cClockHz);
  Wire.setTimeOut(kTouchWireTimeoutMs);
  wakeController();

  if (!detectAddress() || !configureTouch()) {
    Serial.println("Touch GT911: recuperacao falhou, nova tentativa depois.");
    return false;
  }

  ready = true;
  nextRead = millis();
  nextHealthCheckMs = millis() + kTouchHealthIntervalMs;
  Serial.printf("Touch GT911: recuperado no endereco 0x%02X\n", touchAddress);
  return false;
}
