#include "sensors/Bmp280Barometer.h"

#include <Wire.h>
#include <math.h>
#include <string.h>

#include "system/I2CBusLock.h"

namespace {

static constexpr uint32_t kRealtimeTaskIntervalMs = 5;
static constexpr uint32_t kRealtimeTaskStopTimeoutMs = 150;
static constexpr uint32_t kRealtimeTaskStackBytes = 4096;
// Above the Arduino loop (UI), below the dedicated audio task.
static constexpr UBaseType_t kRealtimeTaskPriority = 2;

uint16_t readU16LE(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

int16_t readS16LE(const uint8_t* data) {
  return static_cast<int16_t>(readU16LE(data));
}

float chooseDisplayAgl(float baroAglM, float gpsAglM, bool gpsFix) {
  if (gpsFix && baroAglM < 3.0F && gpsAglM > 3.0F) {
    return gpsAglM;
  }
  return baroAglM;
}

}  // namespace

bool Bmp280Barometer::begin() {
  ready_ = false;
  status_ = BarometerSensorStatus::Off;
  sampleValid_ = false;
  groundReferenceSet_ = false;
  sampleCount_ = 0;
  failedReadCount_ = 0;
  lastSampleMs_ = 0;
  lastHistoryMs_ = 0;
  historyCount_ = 0;
  audioVarioMs_ = 0.0F;
  groundAltitudeReferenceM_ = 0.0F;
  memset(history_, 0, sizeof(history_));

  if (!detect()) {
    Serial.println("BMP280/BME280 nao encontrado no I2C");
    printI2cScan(Serial);
    status_ = BarometerSensorStatus::NotFound;
    return false;
  }
  if (!readCalibration()) {
    Serial.println("Falha ao ler calibracao do BMP280/BME280");
    status_ = BarometerSensorStatus::CalibrationError;
    return false;
  }
  if (!configure()) {
    Serial.println("Falha ao configurar BMP280/BME280");
    status_ = BarometerSensorStatus::ConfigError;
    return false;
  }

  ready_ = true;
  status_ = BarometerSensorStatus::NoSample;
  delay(60);
  readSample(millis());
  Serial.printf("BMP280/BME280 iniciado no endereco 0x%02X chipId=0x%02X\n", address_, chipId_);
  return true;
}

void Bmp280Barometer::end() {
  stopRealtimePolling();
  if (ready_ && address_ != 0) {
    // BMP280/BME280 ctrl_meas[1:0] = 00: sleep mode before ESP deep sleep.
    writeRegister(0xF4, 0x00);
  }
  portENTER_CRITICAL(&dataMux_);
  ready_ = false;
  sampleValid_ = false;
  status_ = BarometerSensorStatus::Off;
  audioVarioMs_ = 0.0F;
  portEXIT_CRITICAL(&dataMux_);
}

bool Bmp280Barometer::poll() {
  if (!ready_) return false;

  const uint32_t now = millis();
  if (lastSampleMs_ != 0 && now - lastSampleMs_ < kSampleIntervalMs) {
    return false;
  }
  return readSample(now);
}

bool Bmp280Barometer::startRealtimePolling(RealtimeSampleCallback callback, void* context) {
  if (!ready_) {
    return false;
  }
  if (pollingTaskHandle_ != nullptr) {
    realtimeSampleCallback_ = callback;
    realtimeSampleContext_ = context;
    return true;
  }

  realtimeSampleCallback_ = callback;
  realtimeSampleContext_ = context;
  pollingStopRequested_ = false;

  TaskHandle_t createdHandle = nullptr;
#if CONFIG_FREERTOS_UNICORE
  const BaseType_t created =
      xTaskCreate(pollingTaskEntry, "baro_rt", kRealtimeTaskStackBytes, this, kRealtimeTaskPriority, &createdHandle);
#else
  const BaseType_t created =
      xTaskCreatePinnedToCore(pollingTaskEntry, "baro_rt", kRealtimeTaskStackBytes, this, kRealtimeTaskPriority, &createdHandle, 0);
#endif
  if (created != pdPASS) {
    realtimeSampleCallback_ = nullptr;
    realtimeSampleContext_ = nullptr;
    pollingTaskHandle_ = nullptr;
    Serial.println("BARO realtime: tarefa nao iniciou; usando fallback no loop.");
    return false;
  }

  pollingTaskHandle_ = createdHandle;
  Serial.println("BARO realtime: leitura dedicada a 20 Hz iniciada.");
  return true;
}

bool Bmp280Barometer::hasSample() const {
  portENTER_CRITICAL(&dataMux_);
  const bool valid = sampleValid_;
  portEXIT_CRITICAL(&dataMux_);
  return valid;
}

bool Bmp280Barometer::audioDataValid() const {
  portENTER_CRITICAL(&dataMux_);
  const bool ready = ready_;
  const bool valid = sampleValid_;
  const uint32_t lastSample = lastSampleMs_;
  portEXIT_CRITICAL(&dataMux_);
  return ready && valid && lastSample != 0 && millis() - lastSample <= kAudioStaleSampleMs;
}

float Bmp280Barometer::audioVarioMs() const {
  portENTER_CRITICAL(&dataMux_);
  const float value = audioVarioMs_;
  portEXIT_CRITICAL(&dataMux_);
  return value;
}

uint32_t Bmp280Barometer::taskStackFreeBytes() const {
  TaskHandle_t handle = pollingTaskHandle_;
  if (handle == nullptr) {
    return 0;
  }
  return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(handle));
}

void Bmp280Barometer::applyTo(VarioData& data) const {
  bool ready = false;
  bool sampleValid = false;
  bool groundReferenceSet = false;
  uint8_t address = 0;
  uint8_t chipId = 0;
  BarometerSensorStatus status = BarometerSensorStatus::Off;
  uint32_t sampleCount = 0;
  uint32_t failedReadCount = 0;
  uint32_t lastSample = 0;
  float filteredAltitudeM = 0.0F;
  float filteredVarioMs = 0.0F;
  float groundAltitudeReferenceM = 0.0F;
  float pressureHpa = 0.0F;
  float temperatureC = 0.0F;
  float history[kVarioHistorySamples] = {};
  uint8_t historyCount = 0;

  portENTER_CRITICAL(&dataMux_);
  ready = ready_;
  sampleValid = sampleValid_;
  groundReferenceSet = groundReferenceSet_;
  address = address_;
  chipId = chipId_;
  status = status_;
  sampleCount = sampleCount_;
  failedReadCount = failedReadCount_;
  lastSample = lastSampleMs_;
  filteredAltitudeM = filter_.altitudeM();
  filteredVarioMs = filter_.varioMs();
  groundAltitudeReferenceM = groundAltitudeReferenceM_;
  pressureHpa = pressureHpa_;
  temperatureC = temperatureC_;
  memcpy(history, history_, sizeof(history));
  historyCount = historyCount_;
  portEXIT_CRITICAL(&dataMux_);

  const uint32_t sampleAge = lastSample == 0 ? UINT32_MAX : millis() - lastSample;
  data.barometerSampleAgeMs = sampleAge;
  data.barometerFailedReads = failedReadCount;
  data.barometerSampleCount = sampleCount;
  data.barometerI2cAddress = address;
  data.barometerChipId = chipId;
  if (!ready || status == BarometerSensorStatus::Off || status == BarometerSensorStatus::NotFound ||
      status == BarometerSensorStatus::CalibrationError || status == BarometerSensorStatus::ConfigError) {
    data.barometerStatus = status;
  } else if (!sampleValid) {
    data.barometerStatus = BarometerSensorStatus::NoSample;
  } else if (sampleAge > kStaleSampleMs) {
    data.barometerStatus = BarometerSensorStatus::StaleSample;
  } else if (status == BarometerSensorStatus::ReadError) {
    data.barometerStatus = BarometerSensorStatus::ReadError;
  } else {
    data.barometerStatus = BarometerSensorStatus::Ok;
  }

  if (!sampleValid) {
    data.varioMs = 0.0F;
    data.ganhoTermicaM = 0.0F;
    data.glideRatio = 0.0F;
    data.altitudeM = 0.0F;
    data.altitudeBaroAglM = 0.0F;
    data.pressureHpa = 0.0F;
    data.temperatureC = 0.0F;
    data.sensorDataValid = false;
    data.historyCount = 0;
    memset(data.varioHistory, 0, sizeof(data.varioHistory));
    return;
  }

  data.altitudeM = filteredAltitudeM;
  float aglM = groundReferenceSet ? filteredAltitudeM - groundAltitudeReferenceM : 0.0F;
  if (aglM < 2.0F) {
    aglM = 0.0F;
  }
  data.altitudeBaroAglM = aglM;
  data.altitudeAglM = chooseDisplayAgl(aglM, data.altitudeGpsAglM, data.gpsFix);
  data.varioMs = filteredVarioMs;
  data.pressureHpa = pressureHpa;
  data.qnhHpa = kDefaultQnhHpa;
  data.temperatureC = temperatureC;
  data.sensorDataValid = data.barometerStatus == BarometerSensorStatus::Ok;
  memcpy(data.varioHistory, history, sizeof(history));
  data.historyCount = historyCount;

}

void Bmp280Barometer::printStatus(Stream& out) const {
  out.print("BARO: ");
  out.print(ready_ ? "READY" : "OFF");
  out.print(" sample=");
  out.print(sampleValid_ ? "OK" : "NO");
  out.print(" addr=0x");
  out.print(address_, HEX);
  out.print(" chip=0x");
  out.print(chipId_, HEX);
  out.print(" count=");
  out.print(sampleCount_);
  out.print(" fail=");
  out.print(failedReadCount_);
  out.print(" p=");
  out.print(pressureHpa_, 2);
  out.print("hPa rawAlt=");
  out.print(rawAltitudeM_, 2);
  out.print("m filtAlt=");
  out.print(filter_.altitudeM(), 1);
  out.print("m vario=");
  out.print(filter_.varioMs(), 3);
  out.print("m/s temp=");
  out.print(temperatureC_, 1);
  out.print("C ground=");
  out.print(groundAltitudeReferenceM_, 1);
  out.print("m age=");
  out.print(lastSampleMs_ == 0 ? 0UL : millis() - lastSampleMs_);
  out.println("ms");
}

bool Bmp280Barometer::detect() {
  const uint8_t addresses[] = {kAddressLow, kAddressHigh};
  for (uint8_t candidate : addresses) {
    address_ = candidate;
    uint8_t chipId = 0;
    if (readRegister(0xD0, chipId)) {
      Serial.printf("BARO detect: endereco 0x%02X respondeu chipId=0x%02X\n", candidate, chipId);
      if (chipId == kBmp280ChipId || chipId == kBme280ChipId) {
        chipId_ = chipId;
        return true;
      }
    } else {
      Serial.printf("BARO detect: endereco 0x%02X sem resposta\n", candidate);
    }
  }
  address_ = 0;
  chipId_ = 0;
  return false;
}

void Bmp280Barometer::printI2cScan(Stream& out) {
  I2CBusGuard bus(pdMS_TO_TICKS(250));
  if (!bus.locked()) {
    out.println("I2C scan: barramento ocupado");
    return;
  }

  out.print("I2C scan:");
  bool found = false;
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      out.print(" 0x");
      if (address < 16) out.print("0");
      out.print(address, HEX);
      found = true;
    }
  }
  if (!found) {
    out.print(" nenhum dispositivo encontrado");
  }
  out.println();
}

bool Bmp280Barometer::configure() {
  if (!writeRegister(0xE0, 0xB6)) return false;
  delay(10);

  // IIR x4, temperature x1, pressure x16, normal mode.
  if (!writeRegister(0xF5, 0x08)) return false;
  return writeRegister(0xF4, 0x37);
}

bool Bmp280Barometer::readCalibration() {
  uint8_t calib[24] = {};
  if (!readBytes(0x88, calib, sizeof(calib))) {
    return false;
  }

  digT1_ = readU16LE(&calib[0]);
  digT2_ = readS16LE(&calib[2]);
  digT3_ = readS16LE(&calib[4]);
  digP1_ = readU16LE(&calib[6]);
  digP2_ = readS16LE(&calib[8]);
  digP3_ = readS16LE(&calib[10]);
  digP4_ = readS16LE(&calib[12]);
  digP5_ = readS16LE(&calib[14]);
  digP6_ = readS16LE(&calib[16]);
  digP7_ = readS16LE(&calib[18]);
  digP8_ = readS16LE(&calib[20]);
  digP9_ = readS16LE(&calib[22]);
  return digP1_ != 0;
}

bool Bmp280Barometer::readSample(uint32_t now) {
  int32_t adcT = 0;
  int32_t adcP = 0;
  if (!readRaw(adcT, adcP)) {
    portENTER_CRITICAL(&dataMux_);
    ++failedReadCount_;
    status_ = sampleValid_ ? BarometerSensorStatus::ReadError : BarometerSensorStatus::NoSample;
    portEXIT_CRITICAL(&dataMux_);
    return false;
  }

  const int32_t tempHundredthsC = compensateTemperature(adcT);
  const uint32_t pressureQ24 = compensatePressure(adcP);
  if (pressureQ24 == 0) {
    portENTER_CRITICAL(&dataMux_);
    ++failedReadCount_;
    status_ = sampleValid_ ? BarometerSensorStatus::ReadError : BarometerSensorStatus::NoSample;
    portEXIT_CRITICAL(&dataMux_);
    return false;
  }

  portENTER_CRITICAL(&dataMux_);
  temperatureC_ = static_cast<float>(tempHundredthsC) / 100.0F;
  const float pressurePa = static_cast<float>(pressureQ24) / 256.0F;
  pressureHpa_ = pressurePa / 100.0F;
  rawAltitudeM_ = pressureToAltitude(pressureHpa_);

  const float nowSeconds = static_cast<float>(now) / 1000.0F;
  filter_.update(rawAltitudeM_, nowSeconds);
  audioVarioMs_ = filter_.varioMs();
  if (!groundReferenceSet_) {
    groundAltitudeReferenceM_ = filter_.altitudeM();
    groundReferenceSet_ = true;
  }

  lastSampleMs_ = now;
  ++sampleCount_;
  sampleValid_ = true;
  status_ = BarometerSensorStatus::Ok;
  pushHistory(filter_.varioMs(), now);
  portEXIT_CRITICAL(&dataMux_);
  return true;
}

bool Bmp280Barometer::readRaw(int32_t& adcT, int32_t& adcP) {
  uint8_t data[6] = {};
  if (!readBytes(0xF7, data, sizeof(data))) {
    return false;
  }

  adcP = (static_cast<int32_t>(data[0]) << 12) | (static_cast<int32_t>(data[1]) << 4) | (static_cast<int32_t>(data[2]) >> 4);
  adcT = (static_cast<int32_t>(data[3]) << 12) | (static_cast<int32_t>(data[4]) << 4) | (static_cast<int32_t>(data[5]) >> 4);
  return true;
}

int32_t Bmp280Barometer::compensateTemperature(int32_t adcT) {
  const int32_t var1 = (((adcT >> 3) - (static_cast<int32_t>(digT1_) << 1)) * static_cast<int32_t>(digT2_)) >> 11;
  const int32_t var2 = (((((adcT >> 4) - static_cast<int32_t>(digT1_)) * ((adcT >> 4) - static_cast<int32_t>(digT1_))) >> 12) *
                        static_cast<int32_t>(digT3_)) >>
                       14;
  tFine_ = var1 + var2;
  return (tFine_ * 5 + 128) >> 8;
}

uint32_t Bmp280Barometer::compensatePressure(int32_t adcP) {
  int64_t var1 = static_cast<int64_t>(tFine_) - 128000;
  int64_t var2 = var1 * var1 * static_cast<int64_t>(digP6_);
  var2 += (var1 * static_cast<int64_t>(digP5_)) << 17;
  var2 += static_cast<int64_t>(digP4_) << 35;
  var1 = ((var1 * var1 * static_cast<int64_t>(digP3_)) >> 8) + ((var1 * static_cast<int64_t>(digP2_)) << 12);
  var1 = (((static_cast<int64_t>(1) << 47) + var1) * static_cast<int64_t>(digP1_)) >> 33;
  if (var1 == 0) {
    return 0;
  }

  int64_t pressure = 1048576 - adcP;
  pressure = (((pressure << 31) - var2) * 3125) / var1;
  var1 = (static_cast<int64_t>(digP9_) * (pressure >> 13) * (pressure >> 13)) >> 25;
  var2 = (static_cast<int64_t>(digP8_) * pressure) >> 19;
  pressure = ((pressure + var1 + var2) >> 8) + (static_cast<int64_t>(digP7_) << 4);
  return static_cast<uint32_t>(pressure);
}

float Bmp280Barometer::pressureToAltitude(float pressureHpa) const {
  if (pressureHpa <= 0.0F) {
    return 0.0F;
  }
  return 44330.0F * (1.0F - powf(pressureHpa / kDefaultQnhHpa, 0.190294957F));
}

void Bmp280Barometer::pushHistory(float varioMs, uint32_t now) {
  if (lastHistoryMs_ != 0 && now - lastHistoryMs_ < 500UL) {
    return;
  }
  lastHistoryMs_ = now;

  for (uint8_t i = 1; i < kVarioHistorySamples; ++i) {
    history_[i - 1] = history_[i];
  }
  history_[kVarioHistorySamples - 1] = varioMs;
  if (historyCount_ < kVarioHistorySamples) {
    ++historyCount_;
  }
}

bool Bmp280Barometer::writeRegister(uint8_t reg, uint8_t value) {
  I2CBusGuard bus(pdMS_TO_TICKS(40));
  if (!bus.locked()) {
    return false;
  }

  Wire.beginTransmission(address_);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool Bmp280Barometer::readRegister(uint8_t reg, uint8_t& value) {
  return readBytes(reg, &value, 1);
}

bool Bmp280Barometer::readBytes(uint8_t reg, uint8_t* buffer, uint8_t length) {
  I2CBusGuard bus(pdMS_TO_TICKS(40));
  if (!bus.locked()) {
    return false;
  }

  Wire.beginTransmission(address_);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  const uint8_t received = Wire.requestFrom(static_cast<int>(address_), static_cast<int>(length));
  if (received != length) {
    return false;
  }

  for (uint8_t i = 0; i < length; ++i) {
    buffer[i] = Wire.read();
  }
  return true;
}

void Bmp280Barometer::stopRealtimePolling() {
  TaskHandle_t handle = pollingTaskHandle_;
  if (handle == nullptr) {
    return;
  }

  pollingStopRequested_ = true;
  const uint32_t startedMs = millis();
  while (pollingTaskHandle_ != nullptr && millis() - startedMs < kRealtimeTaskStopTimeoutMs) {
    delay(1);
  }

  handle = pollingTaskHandle_;
  if (handle != nullptr) {
    vTaskDelete(handle);
    pollingTaskHandle_ = nullptr;
    Serial.println("BARO realtime: parada forcada apos timeout.");
  }
  realtimeSampleCallback_ = nullptr;
  realtimeSampleContext_ = nullptr;
}

void Bmp280Barometer::pollingTaskEntry(void* parameter) {
  Bmp280Barometer* barometer = static_cast<Bmp280Barometer*>(parameter);
  TickType_t lastWake = xTaskGetTickCount();
  uint32_t lastAudioPublishMs = 0;

  while (barometer && !barometer->pollingStopRequested_) {
    const bool sampled = barometer->poll();
    RealtimeSampleCallback callback = barometer->realtimeSampleCallback_;
    const uint32_t now = millis();
    const bool audioFresh = barometer->audioDataValid();
    const bool republishDue = lastAudioPublishMs == 0 || now - lastAudioPublishMs >= kAudioRepublishMs;
    if (callback && (sampled || republishDue || !audioFresh)) {
      lastAudioPublishMs = now;
      const bool valid = barometer->audioDataValid();
      callback(valid ? barometer->audioVarioMs() : 0.0F, valid, barometer->realtimeSampleContext_);
    }
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(kRealtimeTaskIntervalMs));
  }

  if (barometer) {
    barometer->pollingTaskHandle_ = nullptr;
  }
  vTaskDelete(nullptr);
}
