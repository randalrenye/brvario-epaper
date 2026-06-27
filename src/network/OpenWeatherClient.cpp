#include "network/OpenWeatherClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "config/OpenWeatherConfig.h"
#include "network/WifiManager.h"

namespace {

static constexpr uint32_t kWeatherFetchTaskStack = 12288UL;
static constexpr UBaseType_t kWeatherFetchTaskPriority = 1;
// The free OpenWeather 5 day / 3 hour endpoint returns up to 40 timestamps.
static constexpr uint8_t kForecastRequestRows = 40;

float clampFloat(float value, float lo, float hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

int clampInt(int value, int lo, int hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

float msToKmh(float value) {
  return value * 3.6F;
}

void copyText(char* dst, size_t size, const char* src) {
  if (!dst || size == 0) return;
  snprintf(dst, size, "%s", src ? src : "");
}

float optionalFloat(JsonVariantConst value, float fallback = 0.0F) {
  return value.is<float>() || value.is<int>() ? value.as<float>() : fallback;
}

uint8_t optionalPercent(JsonVariantConst value, uint8_t fallback = 0) {
  if (value.is<float>() || value.is<int>()) {
    const float raw = value.as<float>();
    const float percent = raw <= 1.0F ? raw * 100.0F : raw;
    return static_cast<uint8_t>(clampInt(static_cast<int>(percent + 0.5F), 0, 100));
  }
  return fallback;
}

String normalizedApiKey() {
  String key = OpenWeatherConfig::kApiKey;
  key.replace("\\", "");
  key.replace("\"", "");
  key.trim();
  return key;
}

uint32_t localDayStartUtc(uint32_t utc, int32_t timezoneOffsetSeconds) {
  const int64_t localTime = static_cast<int64_t>(utc) + static_cast<int64_t>(timezoneOffsetSeconds);
  const int64_t localDayStart = (localTime / 86400LL) * 86400LL;
  return static_cast<uint32_t>(localDayStart - static_cast<int64_t>(timezoneOffsetSeconds));
}

uint32_t pilotWindowStartUtc(uint32_t referenceUtc, int32_t timezoneOffsetSeconds) {
  static constexpr uint32_t kPilotStartHour = 9UL;
  return localDayStartUtc(referenceUtc, timezoneOffsetSeconds) + kPilotStartHour * 3600UL;
}

uint32_t pilotWindowEndUtc(uint32_t referenceUtc, int32_t timezoneOffsetSeconds) {
  static constexpr uint32_t kPilotEndHour = 17UL;
  return localDayStartUtc(referenceUtc, timezoneOffsetSeconds) + kPilotEndHour * 3600UL;
}

void clampToPilotWindow(uint32_t referenceUtc, int32_t timezoneOffsetSeconds, uint32_t& startUtc, uint32_t& endUtc) {
  const uint32_t pilotStart = pilotWindowStartUtc(referenceUtc, timezoneOffsetSeconds);
  const uint32_t pilotEnd = pilotWindowEndUtc(referenceUtc, timezoneOffsetSeconds);
  if (startUtc < pilotStart) startUtc = pilotStart;
  if (endUtc > pilotEnd) endUtc = pilotEnd;
  if (endUtc <= startUtc) {
    startUtc = pilotStart;
    endUtc = pilotEnd;
  }
}

uint32_t chooseFlightWindow(uint32_t nowUtc,
                            uint32_t sunriseUtc,
                            uint32_t sunsetUtc,
                            int32_t timezoneOffsetSeconds,
                            uint32_t& startUtc,
                            uint32_t& endUtc) {
  static constexpr uint32_t kDaySeconds = 86400UL;
  if (sunriseUtc == 0 || sunsetUtc == 0 || sunsetUtc <= sunriseUtc) {
    uint32_t referenceUtc = nowUtc;
    if (nowUtc >= pilotWindowEndUtc(nowUtc, timezoneOffsetSeconds)) {
      referenceUtc += kDaySeconds;
    }
    startUtc = pilotWindowStartUtc(referenceUtc, timezoneOffsetSeconds);
    endUtc = pilotWindowEndUtc(referenceUtc, timezoneOffsetSeconds);
    return startUtc;
  }

  if (nowUtc > sunsetUtc || nowUtc >= pilotWindowEndUtc(nowUtc, timezoneOffsetSeconds)) {
    startUtc = sunriseUtc + kDaySeconds;
    endUtc = sunsetUtc + kDaySeconds;
  } else {
    startUtc = sunriseUtc;
    endUtc = sunsetUtc;
  }
  clampToPilotWindow(startUtc, timezoneOffsetSeconds, startUtc, endUtc);
  return startUtc;
}

uint32_t chooseFlightWindowDay(uint32_t nowUtc,
                               uint32_t sunriseUtc,
                               uint32_t sunsetUtc,
                               int32_t timezoneOffsetSeconds,
                               uint8_t dayIndex,
                               uint32_t& startUtc,
                               uint32_t& endUtc) {
  static constexpr uint32_t kImmediateWindowSeconds = 3UL * 3600UL;
  static constexpr uint32_t kDaySeconds = 86400UL;
  if (dayIndex == 0) {
    if (nowUtc > 0) {
      startUtc = nowUtc;
      endUtc = nowUtc + kImmediateWindowSeconds;
      return startUtc;
    }
    return chooseFlightWindow(nowUtc, sunriseUtc, sunsetUtc, timezoneOffsetSeconds, startUtc, endUtc);
  }

  if (sunriseUtc == 0 || sunsetUtc == 0 || sunsetUtc <= sunriseUtc) {
    const uint32_t referenceUtc = nowUtc + static_cast<uint32_t>(dayIndex) * kDaySeconds;
    startUtc = pilotWindowStartUtc(referenceUtc, timezoneOffsetSeconds);
    endUtc = pilotWindowEndUtc(referenceUtc, timezoneOffsetSeconds);
    return startUtc;
  }

  const uint32_t dayOffset = static_cast<uint32_t>(dayIndex);
  startUtc = sunriseUtc + dayOffset * kDaySeconds;
  endUtc = sunsetUtc + dayOffset * kDaySeconds;
  clampToPilotWindow(startUtc, timezoneOffsetSeconds, startUtc, endUtc);
  return startUtc;
}

void formatForecastDate(char* dst, size_t size, uint32_t startUtc, uint32_t endUtc, int32_t timezoneOffsetSeconds) {
  if (!dst || size == 0) return;
  time_t localStart = static_cast<time_t>(startUtc + timezoneOffsetSeconds);
  time_t localEnd = static_cast<time_t>(endUtc + timezoneOffsetSeconds);
  tm startTm = {};
  tm endTm = {};
  gmtime_r(&localStart, &startTm);
  gmtime_r(&localEnd, &endTm);
  snprintf(dst,
           size,
           "%02d/%02d %02d-%02dH",
           startTm.tm_mday,
           startTm.tm_mon + 1,
           startTm.tm_hour,
           endTm.tm_hour);
}

void formatLocalHourMinute(char* dst, size_t size, uint32_t utc, int32_t timezoneOffsetSeconds) {
  if (!dst || size == 0) return;
  time_t localTime = static_cast<time_t>(utc + timezoneOffsetSeconds);
  tm localTm = {};
  gmtime_r(&localTime, &localTm);
  snprintf(dst, size, "%02d:%02d", localTm.tm_hour, localTm.tm_min);
}

float estimateCloudTopMeters(const WeatherFlightData& data) {
  if (data.cloudBaseMeters <= 0.0F) {
    return 0.0F;
  }

  const float tempRange = data.maxTemperature > data.minTemperature ? data.maxTemperature - data.minTemperature : 0.0F;
  float liftDepth = 520.0F + tempRange * 85.0F + (100.0F - static_cast<float>(data.humidity)) * 7.0F;
  if (data.cloudCover >= 15 && data.cloudCover <= 65) {
    liftDepth += 260.0F;
  }
  if (data.maxRainProbability > 30) {
    liftDepth -= static_cast<float>(data.maxRainProbability - 30) * 10.0F;
  }
  if (data.maxCloudCover > 80) {
    liftDepth -= static_cast<float>(data.maxCloudCover - 80) * 9.0F;
  }
  liftDepth = clampFloat(liftDepth, 350.0F, 1900.0F);
  return data.cloudBaseMeters + liftDepth;
}

float estimateThermalStrengthMs(const WeatherFlightData& data) {
  const float spread = data.temperature - data.dewPoint;
  const float tempRange = data.maxTemperature > data.minTemperature ? data.maxTemperature - data.minTemperature : 0.0F;
  float strength = 0.25F + spread * 0.11F + tempRange * 0.13F;
  strength += clampFloat((data.cloudBaseMeters - 700.0F) / 900.0F, 0.0F, 1.2F);
  if (data.windSpeedKmh < 5.0F) strength -= 0.25F;
  if (data.maxRainProbability > 25) strength -= static_cast<float>(data.maxRainProbability - 25) * 0.018F;
  if (data.maxCloudCover > 75) strength -= static_cast<float>(data.maxCloudCover - 75) * 0.025F;
  return clampFloat(strength, 0.0F, 5.5F);
}

void classifyThermal(char* dst, size_t size, float thermalMs) {
  if (!dst || size == 0) return;
  if (thermalMs >= 3.5F) {
    copyText(dst, size, "FORTE");
  } else if (thermalMs >= 2.0F) {
    copyText(dst, size, "BOA");
  } else if (thermalMs >= 1.0F) {
    copyText(dst, size, "MODERADA");
  } else if (thermalMs > 0.2F) {
    copyText(dst, size, "FRACA");
  } else {
    copyText(dst, size, "INATIVA");
  }
}

void buildFlightSummary(WeatherFlightData& data) {
  if (data.cloudBaseMeters < 800.0F || data.humidity > 88) {
    copyText(data.flightSummary, sizeof(data.flightSummary), "Teto baixo. Alta umidade. Avaliar base e visibilidade.");
  } else if (data.maxRainProbability > 35 || data.rainMm > 0.3F) {
    copyText(data.flightSummary, sizeof(data.flightSummary), "Chuva na janela. Risco de desenvolvimento e interrupçao.");
  } else if (data.windGustKmh > 32.0F || data.windSpeedKmh > 28.0F) {
    copyText(data.flightSummary, sizeof(data.flightSummary), "Vento/rajada forte. Checar rotor, relevo e decolagem.");
  } else if (data.flightIndex >= 80 && data.thermalStrengthMs >= 2.0F) {
    copyText(data.flightSummary, sizeof(data.flightSummary), "Base alta. Termicas boas. Janela favoravel para XC.");
  } else if (data.flightIndex >= 65) {
    copyText(data.flightSummary, sizeof(data.flightSummary), "Boa janela de voo. Termicas utilizaveis. Avaliar local.");
  } else if (data.windSpeedKmh < 5.0F) {
    copyText(data.flightSummary, sizeof(data.flightSummary), "Vento fraco. Rendimento pode depender de termicas fortes.");
  } else {
    copyText(data.flightSummary, sizeof(data.flightSummary), "Condiçao tecnica. Conferir vento, teto e nuvens.");
  }
}

int hourlyFlightScore(const WeatherHourlyForecast& forecast) {
  int score = 100;

  if (forecast.windSpeedKmh < 5.0F) {
    score -= static_cast<int>((5.0F - forecast.windSpeedKmh) * 8.0F);
  } else if (forecast.windSpeedKmh > 22.0F) {
    score -= static_cast<int>((forecast.windSpeedKmh - 22.0F) * 5.0F);
  }
  if (forecast.windSpeedKmh > 30.0F) {
    score -= 18;
  }
  if (forecast.rainProbability > 30) {
    score -= static_cast<int>(forecast.rainProbability - 30) * 2 + 16;
  } else if (forecast.rainProbability > 15) {
    score -= static_cast<int>(forecast.rainProbability - 15);
  }
  if (forecast.cloudCover < 10) {
    score -= 6;
  } else if (forecast.cloudCover > 70) {
    score -= static_cast<int>(forecast.cloudCover - 70);
  }
  if (forecast.temperature < 12.0F) {
    score -= static_cast<int>((12.0F - forecast.temperature) * 2.0F);
  } else if (forecast.temperature > 38.0F) {
    score -= static_cast<int>((forecast.temperature - 38.0F) * 2.0F);
  }

  return clampInt(score, 0, 100);
}

uint32_t hourlySegmentEndUtc(const WeatherFlightData& data, uint8_t index) {
  static constexpr uint32_t kForecastStepSeconds = 3UL * 3600UL;
  uint32_t endUtc = data.hourly[index].timeUtc + kForecastStepSeconds;
  if (index + 1 < data.hourlyCount && data.hourly[index + 1].timeUtc > data.hourly[index].timeUtc) {
    endUtc = data.hourly[index + 1].timeUtc;
  }
  if (data.forecastEndUtc > data.forecastStartUtc && endUtc > data.forecastEndUtc) {
    endUtc = data.forecastEndUtc;
  }
  return endUtc;
}

void chooseLocalFlightWindow(const WeatherFlightData& data, uint32_t& startUtc, uint32_t& peakUtc, uint32_t& endUtc) {
  static constexpr int kUsableScore = 55;
  startUtc = data.forecastStartUtc;
  endUtc = data.forecastEndUtc;
  peakUtc = data.forecastStartUtc;
  if (data.forecastEndUtc > data.forecastStartUtc) {
    peakUtc = data.forecastStartUtc + (data.forecastEndUtc - data.forecastStartUtc) / 2UL;
  }
  if (data.hourlyCount == 0) {
    return;
  }

  int scores[kWeatherHourlyForecastCount] = {};
  int bestPeakScore = -1;
  uint8_t bestPeakIndex = 0;
  for (uint8_t i = 0; i < data.hourlyCount && i < kWeatherHourlyForecastCount; ++i) {
    scores[i] = hourlyFlightScore(data.hourly[i]);
    if (scores[i] > bestPeakScore) {
      bestPeakScore = scores[i];
      bestPeakIndex = i;
    }
  }

  int currentStart = -1;
  int currentScore = 0;
  uint8_t currentCount = 0;
  int bestStart = -1;
  int bestEnd = -1;
  int bestAverage = -1;
  uint8_t bestCount = 0;

  auto closeGroup = [&]() {
    if (currentStart < 0 || currentCount == 0) return;
    const int average = currentScore / static_cast<int>(currentCount);
    if (average > bestAverage || (average == bestAverage && currentCount > bestCount)) {
      bestAverage = average;
      bestCount = currentCount;
      bestStart = currentStart;
      bestEnd = currentStart + static_cast<int>(currentCount) - 1;
    }
    currentStart = -1;
    currentScore = 0;
    currentCount = 0;
  };

  for (uint8_t i = 0; i < data.hourlyCount && i < kWeatherHourlyForecastCount; ++i) {
    if (scores[i] >= kUsableScore) {
      if (currentStart < 0) currentStart = i;
      currentScore += scores[i];
      ++currentCount;
    } else {
      closeGroup();
    }
  }
  closeGroup();

  if (bestStart >= 0 && bestEnd >= bestStart) {
    startUtc = data.hourly[bestStart].timeUtc;
    endUtc = hourlySegmentEndUtc(data, static_cast<uint8_t>(bestEnd));
    bestPeakScore = -1;
    bestPeakIndex = static_cast<uint8_t>(bestStart);
    for (uint8_t i = static_cast<uint8_t>(bestStart); i <= static_cast<uint8_t>(bestEnd); ++i) {
      if (scores[i] > bestPeakScore) {
        bestPeakScore = scores[i];
        bestPeakIndex = i;
      }
    }
  } else {
    startUtc = data.hourly[bestPeakIndex].timeUtc;
    endUtc = hourlySegmentEndUtc(data, bestPeakIndex);
  }

  if (startUtc < data.forecastStartUtc) startUtc = data.forecastStartUtc;
  if (endUtc > data.forecastEndUtc) endUtc = data.forecastEndUtc;
  if (endUtc <= startUtc) {
    startUtc = data.forecastStartUtc;
    endUtc = data.forecastEndUtc;
  }
  peakUtc = data.hourly[bestPeakIndex].timeUtc;
  if (peakUtc < startUtc || peakUtc > endUtc) {
    peakUtc = startUtc + (endUtc - startUtc) / 2UL;
  }
}

}  // namespace

void OpenWeatherClient::begin(WifiManager* wifi) {
  wifi_ = wifi;
  state_ = OpenWeatherState::Idle;
  hasData_ = false;
  forecastDayCount_ = 0;
  requestPending_ = false;
  forecastPartial_ = false;
  fetchTaskRunning_ = false;
  fetchResultReady_ = false;
  fetchResultSuccess_ = false;
  fetchForecastAvailable_ = false;
  fetchTaskHandle_ = nullptr;
  pendingLocationKey_ = 0;
  requestGeneration_ = 0;
  fetchLocationKey_ = 0;
  fetchGeneration_ = 0;
  dataLat_ = 0.0;
  dataLon_ = 0.0;
  dataLocationKey_ = 0;
  fetchCurrentPayload_ = "";
  fetchForecastPayload_ = "";
  errorText_[0] = '\0';
  fetchErrorText_[0] = '\0';
}

void OpenWeatherClient::request(double lat, double lon, float localPressureHpa, uint32_t locationKey) {
  ++requestGeneration_;
  invalidate();
  if (!isConfigured()) {
    setError("API KEY NAO CONFIGURADA");
    return;
  }
  if (!validLocation(lat, lon)) {
    setError("AGUARDANDO GPS COM LAT/LON");
    return;
  }
  if (!wifi_) {
    setError("WIFI MANAGER AUSENTE");
    return;
  }
  if (wifi_->savedSsid().length() == 0 && !wifi_->isConnected()) {
    setError("CONFIGURE O WIFI PRIMEIRO");
    return;
  }

  pendingLat_ = lat;
  pendingLon_ = lon;
  pendingLocalPressureHpa_ = localPressureHpa;
  pendingLocationKey_ = locationKey;
  requestPending_ = true;
  waitStartedMs_ = millis();
  errorText_[0] = '\0';
  fetchErrorText_[0] = '\0';

  wifi_->setEnabled(true);
  if (!wifi_->isConnected() && wifi_->state() != WifiConnectionState::Connecting) {
    wifi_->connectSaved();
  }
  if (fetchTaskRunning_ || fetchResultReady_) {
    state_ = OpenWeatherState::Fetching;
    Serial.println("OpenWeather: novo local enfileirado; resposta anterior sera descartada.");
    return;
  }
  state_ = OpenWeatherState::WaitingWifi;
  Serial.println("OpenWeather: aguardando WiFi para atualizar previsao.");
}

void OpenWeatherClient::invalidate() {
  hasData_ = false;
  forecastDayCount_ = 0;
  forecastPartial_ = false;
  data_ = {};
  for (uint8_t i = 0; i < kWeatherForecastDayCount; ++i) {
    days_[i] = {};
  }
  dataLat_ = 0.0;
  dataLon_ = 0.0;
  dataLocationKey_ = 0;
}

void OpenWeatherClient::update() {
  if (fetchResultReady_) {
    finishFetchResult();
  }

  if (fetchTaskRunning_) {
    return;
  }

  if (!requestPending_) {
    return;
  }

  if (!wifi_) {
    setError("WIFI MANAGER AUSENTE");
    return;
  }

  if (!wifi_->isConnected()) {
    if (millis() - waitStartedMs_ > OpenWeatherConfig::kWifiWaitTimeoutMs) {
      setError("WIFI NAO CONECTOU");
    }
    return;
  }

  state_ = OpenWeatherState::Fetching;
  requestPending_ = false;
  if (!startFetchTask()) {
    setError("FALHA TAREFA METEO");
  }
}

bool OpenWeatherClient::startFetchTask() {
  if (fetchTaskRunning_ || fetchTaskHandle_ != nullptr) {
    return true;
  }

  fetchLat_ = pendingLat_;
  fetchLon_ = pendingLon_;
  fetchLocalPressureHpa_ = pendingLocalPressureHpa_;
  fetchLocationKey_ = pendingLocationKey_;
  fetchGeneration_ = requestGeneration_;
  requestPending_ = false;
  fetchResultReady_ = false;
  fetchResultSuccess_ = false;
  fetchForecastAvailable_ = false;
  fetchCurrentPayload_ = "";
  fetchForecastPayload_ = "";
  fetchErrorText_[0] = '\0';
  fetchTaskRunning_ = true;
  const BaseType_t created = xTaskCreatePinnedToCore(fetchTaskThunk,
                                                     "owm_fetch",
                                                     kWeatherFetchTaskStack,
                                                     this,
                                                     kWeatherFetchTaskPriority,
                                                     &fetchTaskHandle_,
                                                     0);
  if (created != pdPASS) {
    fetchTaskRunning_ = false;
    fetchTaskHandle_ = nullptr;
    return false;
  }
  return true;
}

void OpenWeatherClient::fetchTaskThunk(void* context) {
  OpenWeatherClient* client = static_cast<OpenWeatherClient*>(context);
  if (client) {
    client->runFetchTask();
  }
  vTaskDelete(nullptr);
}

void OpenWeatherClient::runFetchTask() {
  fetchResultSuccess_ = fetchOpenWeatherPayloads(fetchLat_,
                                                fetchLon_,
                                                fetchCurrentPayload_,
                                                fetchForecastPayload_,
                                                fetchForecastAvailable_,
                                                fetchErrorText_,
                                                sizeof(fetchErrorText_));
  fetchResultReady_ = true;
  fetchTaskHandle_ = nullptr;
  fetchTaskRunning_ = false;
}

const WeatherFlightData& OpenWeatherClient::data(uint8_t dayIndex) const {
  if (forecastDayCount_ == 0 || dayIndex >= forecastDayCount_) {
    return data_;
  }
  return days_[dayIndex];
}

bool OpenWeatherClient::fetchOpenWeatherData(double lat, double lon) {
  ++requestGeneration_;
  pendingLat_ = lat;
  pendingLon_ = lon;
  pendingLocalPressureHpa_ = 0.0F;
  pendingLocationKey_ = 0;
  fetchLat_ = lat;
  fetchLon_ = lon;
  fetchLocalPressureHpa_ = 0.0F;
  fetchLocationKey_ = 0;
  fetchGeneration_ = requestGeneration_;
  invalidate();
  fetchCurrentPayload_ = "";
  fetchForecastPayload_ = "";
  fetchForecastAvailable_ = false;
  fetchResultSuccess_ = fetchOpenWeatherPayloads(lat,
                                                lon,
                                                fetchCurrentPayload_,
                                                fetchForecastPayload_,
                                                fetchForecastAvailable_,
                                                fetchErrorText_,
                                                sizeof(fetchErrorText_));
  fetchResultReady_ = true;
  finishFetchResult();
  return hasData_;
}

bool OpenWeatherClient::fetchOpenWeatherPayloads(double lat,
                                                 double lon,
                                                 String& currentPayload,
                                                 String& forecastPayload,
                                                 bool& forecastAvailable,
                                                 char* errorText,
                                                 size_t errorTextSize) {
  forecastAvailable = false;
  currentPayload = "";
  forecastPayload = "";
  if (!isConfigured()) {
    copyText(errorText, errorTextSize, "API KEY NAO CONFIGURADA");
    return false;
  }

  String currentUrl = "http://api.openweathermap.org/data/2.5/weather?lat=";
  currentUrl += String(lat, 6);
  currentUrl += "&lon=";
  currentUrl += String(lon, 6);
  currentUrl += "&units=";
  currentUrl += OpenWeatherConfig::kUnits;
  currentUrl += "&lang=";
  currentUrl += OpenWeatherConfig::kLanguage;
  currentUrl += "&appid=";
  currentUrl += normalizedApiKey();

  if (!httpGet(currentUrl, currentPayload, errorText, errorTextSize)) {
    return false;
  }

  String forecastUrl = "http://api.openweathermap.org/data/2.5/forecast?lat=";
  forecastUrl += String(lat, 6);
  forecastUrl += "&lon=";
  forecastUrl += String(lon, 6);
  forecastUrl += "&units=";
  forecastUrl += OpenWeatherConfig::kUnits;
  forecastUrl += "&lang=";
  forecastUrl += OpenWeatherConfig::kLanguage;
  forecastUrl += "&appid=";
  forecastUrl += normalizedApiKey();
  forecastUrl += "&cnt=";
  forecastUrl += String(static_cast<unsigned>(kForecastRequestRows));

  char forecastError[80] = {};
  if (!httpGet(forecastUrl, forecastPayload, forecastError, sizeof(forecastError))) {
    Serial.printf("OpenWeather: previsao 3h indisponivel, usando clima atual: %s\n",
                  forecastError[0] != '\0' ? forecastError : "falha desconhecida");
    forecastPayload = "";
    forecastAvailable = false;
    return true;
  }

  forecastAvailable = true;
  return true;
}

void OpenWeatherClient::finishFetchResult() {
  fetchResultReady_ = false;
  if (fetchGeneration_ != requestGeneration_) {
    fetchCurrentPayload_ = "";
    fetchForecastPayload_ = "";
    fetchResultSuccess_ = false;
    fetchForecastAvailable_ = false;
    state_ = requestPending_ ? OpenWeatherState::WaitingWifi : OpenWeatherState::Idle;
    Serial.println("OpenWeather: resposta descartada porque o local mudou.");
    return;
  }

  if (!fetchResultSuccess_) {
    invalidate();
    setError(fetchErrorText_[0] != '\0' ? fetchErrorText_ : "FALHA METEO");
    fetchCurrentPayload_ = "";
    fetchForecastPayload_ = "";
    return;
  }

  state_ = OpenWeatherState::Fetching;
  pendingLocalPressureHpa_ = fetchLocalPressureHpa_;
  forecastPartial_ = !fetchForecastAvailable_;
  parseOpenWeatherJson(fetchCurrentPayload_);
  if (state_ == OpenWeatherState::Error) {
    hasData_ = false;
    fetchCurrentPayload_ = "";
    fetchForecastPayload_ = "";
    return;
  }

  buildCurrentOnlyForecast();
  if (fetchForecastAvailable_ && fetchForecastPayload_.length() > 0) {
    parseForecastJson(fetchForecastPayload_);
    if (state_ == OpenWeatherState::Error || forecastDayCount_ == 0) {
      Serial.printf("OpenWeather: mantendo clima atual; previsao 3h falhou: %s\n", errorText_);
      buildCurrentOnlyForecast();
      forecastPartial_ = true;
      errorText_[0] = '\0';
    } else {
      forecastPartial_ = false;
    }
  }

  const uint32_t updatedAt = millis();
  data_.updatedAtMs = updatedAt;
  if (forecastDayCount_ > 0) {
    days_[0].updatedAtMs = updatedAt;
  }
  state_ = OpenWeatherState::Ready;
  hasData_ = true;
  dataLat_ = fetchLat_;
  dataLon_ = fetchLon_;
  dataLocationKey_ = fetchLocationKey_;
  printWeatherDebug();
  fetchCurrentPayload_ = "";
  fetchForecastPayload_ = "";
}

void OpenWeatherClient::parseOpenWeatherJson(String payload) {
  JsonDocument filter;
  filter["main"]["temp"] = true;
  filter["main"]["feels_like"] = true;
  filter["main"]["pressure"] = true;
  filter["main"]["humidity"] = true;
  filter["wind"]["speed"] = true;
  filter["wind"]["gust"] = true;
  filter["wind"]["deg"] = true;
  filter["clouds"]["all"] = true;
  filter["rain"]["1h"] = true;
  filter["visibility"] = true;
  filter["dt"] = true;
  filter["timezone"] = true;
  filter["sys"]["sunrise"] = true;
  filter["sys"]["sunset"] = true;
  filter["weather"][0]["description"] = true;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (error) {
    setError("DADOS METEO INVALIDOS");
    Serial.printf("OpenWeather JSON erro: %s\n", error.c_str());
    return;
  }

  JsonObjectConst main = doc["main"].as<JsonObjectConst>();
  if (main.isNull()) {
    setError("API SEM DADOS");
    return;
  }

  data_ = {};
  forecastDayCount_ = 0;
  data_.currentUtc = doc["dt"] | 0UL;
  data_.timezoneOffsetSeconds = doc["timezone"] | 0;
  data_.sunriseUtc = doc["sys"]["sunrise"] | 0UL;
  data_.sunsetUtc = doc["sys"]["sunset"] | 0UL;
  chooseFlightWindowDay(data_.currentUtc,
                        data_.sunriseUtc,
                        data_.sunsetUtc,
                        data_.timezoneOffsetSeconds,
                        0,
                        data_.forecastStartUtc,
                        data_.forecastEndUtc);
  formatForecastDate(data_.forecastDateText, sizeof(data_.forecastDateText), data_.forecastStartUtc, data_.forecastEndUtc, data_.timezoneOffsetSeconds);
  data_.temperature = optionalFloat(main["temp"]);
  data_.minTemperature = data_.temperature;
  data_.maxTemperature = data_.temperature;
  data_.feelsLike = optionalFloat(main["feels_like"], data_.temperature);
  data_.humidity = optionalPercent(main["humidity"]);
  data_.apiPressure = optionalFloat(main["pressure"]);
  data_.localPressure = pendingLocalPressureHpa_;
  data_.usingLocalPressure = OpenWeatherConfig::kPreferLocalPressure && pendingLocalPressureHpa_ > 700.0F && pendingLocalPressureHpa_ < 1100.0F;
  data_.pressure = data_.usingLocalPressure ? pendingLocalPressureHpa_ : data_.apiPressure;
  data_.dewPoint = calculateDewPoint(data_.temperature, data_.humidity);
  data_.windSpeedKmh = msToKmh(optionalFloat(doc["wind"]["speed"]));
  data_.windGustKmh = msToKmh(optionalFloat(doc["wind"]["gust"], optionalFloat(doc["wind"]["speed"])));
  data_.windDirectionDeg = doc["wind"]["deg"] | 0;
  copyText(data_.windDirectionText, sizeof(data_.windDirectionText), getWindDirectionText(data_.windDirectionDeg).c_str());
  data_.cloudCover = optionalPercent(doc["clouds"]["all"]);
  data_.rainProbability = 0;
  data_.rainMm = optionalFloat(doc["rain"]["1h"]);
  data_.uvi = -1.0F;
  data_.uviAvailable = false;
  data_.visibility = static_cast<uint16_t>((doc["visibility"] | 0) / 1000);
  const char* description = doc["weather"][0]["description"] | "sem descricao";
  copyText(data_.condition, sizeof(data_.condition), description);
}

void OpenWeatherClient::buildCurrentOnlyForecast() {
  windForecastCount_ = 0;
  for (uint8_t i = 0; i < kWeatherWindForecastCount; ++i) {
    windForecast_[i] = {};
  }
  data_.hourlyCount = 0;
  data_.rainProbability = 0;
  data_.maxRainProbability = data_.rainProbability;
  data_.maxCloudCover = data_.cloudCover;
  data_.maxWindSpeedKmh = data_.windSpeedKmh;
  data_.maxWindGustKmh = data_.windGustKmh;
  data_.minTemperature = data_.temperature;
  data_.maxTemperature = data_.temperature;
  data_.rainMm = data_.rainMm < 0.0F ? 0.0F : data_.rainMm;
  data_.minCloudBaseMeters = 0.0F;
  applyFlightCalculations(data_);

  for (uint8_t i = 0; i < kWeatherForecastDayCount; ++i) {
    days_[i] = {};
  }
  days_[0] = data_;
  forecastDayCount_ = 1;
}

void OpenWeatherClient::parseForecastJson(String payload) {
  JsonDocument filter;
  for (uint8_t i = 0; i < kForecastRequestRows; ++i) {
    filter["list"][i]["dt"] = true;
    filter["list"][i]["main"]["temp"] = true;
    filter["list"][i]["main"]["feels_like"] = true;
    filter["list"][i]["main"]["pressure"] = true;
    filter["list"][i]["main"]["humidity"] = true;
    filter["list"][i]["wind"]["speed"] = true;
    filter["list"][i]["wind"]["gust"] = true;
    filter["list"][i]["wind"]["deg"] = true;
    filter["list"][i]["pop"] = true;
    filter["list"][i]["clouds"]["all"] = true;
    filter["list"][i]["rain"]["3h"] = true;
    filter["list"][i]["visibility"] = true;
    filter["list"][i]["weather"][0]["main"] = true;
    filter["list"][i]["weather"][0]["description"] = true;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (error) {
    setError("DADOS DA PREVISAO INVALIDOS");
    Serial.printf("OpenWeather forecast JSON erro: %s\n", error.c_str());
    return;
  }

  JsonArrayConst rows = doc["list"].as<JsonArrayConst>();
  if (rows.isNull() || rows.size() == 0) {
    setError("PREVISAO SEM DADOS");
    return;
  }

  const WeatherFlightData base = data_;
  forecastDayCount_ = 0;
  windForecastCount_ = 0;
  for (uint8_t i = 0; i < kWeatherForecastDayCount; ++i) {
    days_[i] = {};
  }
  for (uint8_t i = 0; i < kWeatherWindForecastCount; ++i) {
    windForecast_[i] = {};
  }

  for (JsonObjectConst row : rows) {
    if (windForecastCount_ >= kWeatherWindForecastCount) {
      break;
    }
    const uint32_t rowTime = row["dt"] | 0UL;
    if (rowTime == 0) {
      continue;
    }
    WeatherHourlyForecast& forecast = windForecast_[windForecastCount_++];
    forecast.timeUtc = rowTime;
    forecast.temperature = optionalFloat(row["main"]["temp"]);
    forecast.windSpeedKmh = msToKmh(optionalFloat(row["wind"]["speed"]));
    forecast.windDirectionDeg = row["wind"]["deg"] | base.windDirectionDeg;
    forecast.rainProbability = optionalPercent(row["pop"]);
    forecast.cloudCover = optionalPercent(row["clouds"]["all"]);
    copyText(forecast.condition, sizeof(forecast.condition), row["weather"][0]["main"] | "");
  }

  for (uint8_t dayIndex = 0; dayIndex < kWeatherForecastDayCount; ++dayIndex) {
    WeatherFlightData day = base;
    const bool immediateWindow = dayIndex == 0;
    chooseFlightWindowDay(base.currentUtc,
                          base.sunriseUtc,
                          base.sunsetUtc,
                          base.timezoneOffsetSeconds,
                          dayIndex,
                          day.forecastStartUtc,
                          day.forecastEndUtc);
    formatForecastDate(day.forecastDateText, sizeof(day.forecastDateText), day.forecastStartUtc, day.forecastEndUtc, day.timezoneOffsetSeconds);

    uint8_t windowCount = immediateWindow ? 1 : 0;
    bool firstWindowRow = !immediateWindow;
    day.hourlyCount = 0;
    day.rainProbability = immediateWindow ? base.rainProbability : 0;
    day.maxRainProbability = immediateWindow ? base.rainProbability : 0;
    day.maxCloudCover = immediateWindow ? base.cloudCover : 0;
    day.maxWindSpeedKmh = immediateWindow ? base.windSpeedKmh : 0.0F;
    day.maxWindGustKmh = immediateWindow ? base.windGustKmh : 0.0F;
    day.minTemperature = day.temperature;
    day.maxTemperature = day.temperature;
    day.rainMm = immediateWindow && base.rainMm > 0.0F ? base.rainMm : 0.0F;
    day.minCloudBaseMeters = immediateWindow && base.cloudBaseMeters > 0.0F ? base.cloudBaseMeters : 99999.0F;
    uint8_t outIndex = 0;

    if (immediateWindow && outIndex < kWeatherHourlyForecastCount) {
      WeatherHourlyForecast& current = day.hourly[outIndex++];
      current.timeUtc = base.currentUtc;
      current.temperature = base.temperature;
      current.windSpeedKmh = base.windSpeedKmh;
      current.windDirectionDeg = base.windDirectionDeg;
      current.rainProbability = base.rainProbability;
      current.cloudCover = base.cloudCover;
      copyText(current.condition, sizeof(current.condition), base.condition);
    }

    for (JsonObjectConst row : rows) {
      const uint32_t rowTime = row["dt"] | 0UL;
      if (rowTime < day.forecastStartUtc || rowTime > day.forecastEndUtc) {
        continue;
      }

      const float rowTemp = optionalFloat(row["main"]["temp"]);
      const float rowFeelsLike = optionalFloat(row["main"]["feels_like"], rowTemp);
      const uint8_t rowHumidity = optionalPercent(row["main"]["humidity"]);
      const float rowPressure = optionalFloat(row["main"]["pressure"]);
      const float rowWindKmh = msToKmh(optionalFloat(row["wind"]["speed"]));
      const float rowGustKmh = msToKmh(optionalFloat(row["wind"]["gust"], optionalFloat(row["wind"]["speed"])));
      const int rowWindDeg = row["wind"]["deg"] | day.windDirectionDeg;
      const uint8_t rowRainProbability = optionalPercent(row["pop"]);
      const uint8_t rowCloudCover = optionalPercent(row["clouds"]["all"]);
      const float rowRainMm = optionalFloat(row["rain"]["3h"]);
      const float rowDewPoint = calculateDewPoint(rowTemp, rowHumidity);
      const float rowCloudBaseM = calculateCloudBase(rowTemp, rowDewPoint);

      if (firstWindowRow) {
        firstWindowRow = false;
        day.temperature = rowTemp;
        day.minTemperature = rowTemp;
        day.maxTemperature = rowTemp;
        day.feelsLike = rowFeelsLike;
        day.humidity = rowHumidity;
        day.apiPressure = rowPressure;
        const bool currentWindow = dayIndex == 0 && day.currentUtc >= day.forecastStartUtc && day.currentUtc <= day.forecastEndUtc + 900UL;
        day.usingLocalPressure = OpenWeatherConfig::kPreferLocalPressure && currentWindow && pendingLocalPressureHpa_ > 700.0F &&
                                  pendingLocalPressureHpa_ < 1100.0F;
        day.pressure = day.usingLocalPressure ? pendingLocalPressureHpa_ : day.apiPressure;
        day.dewPoint = rowDewPoint;
        day.windSpeedKmh = rowWindKmh;
        day.windGustKmh = rowGustKmh;
        day.windDirectionDeg = rowWindDeg;
        copyText(day.windDirectionText, sizeof(day.windDirectionText), getWindDirectionText(day.windDirectionDeg).c_str());
        day.cloudCover = rowCloudCover;
        day.rainProbability = rowRainProbability;
        day.visibility = static_cast<uint16_t>((row["visibility"] | static_cast<int>(day.visibility) * 1000) / 1000);
        copyText(day.condition, sizeof(day.condition), row["weather"][0]["description"] | row["weather"][0]["main"] | "");
      }

      ++windowCount;
      if (outIndex < kWeatherHourlyForecastCount) {
        WeatherHourlyForecast& forecast = day.hourly[outIndex];
        forecast.timeUtc = rowTime;
        forecast.temperature = rowTemp;
        forecast.windSpeedKmh = rowWindKmh;
        forecast.windDirectionDeg = rowWindDeg;
        forecast.rainProbability = rowRainProbability;
        forecast.cloudCover = rowCloudCover;
        copyText(forecast.condition, sizeof(forecast.condition), row["weather"][0]["main"] | "");
        ++outIndex;
      }

      if (rowRainProbability > day.maxRainProbability) day.maxRainProbability = rowRainProbability;
      if (rowCloudCover > day.maxCloudCover) day.maxCloudCover = rowCloudCover;
      if (rowWindKmh > day.maxWindSpeedKmh) day.maxWindSpeedKmh = rowWindKmh;
      if (rowGustKmh > day.maxWindGustKmh) day.maxWindGustKmh = rowGustKmh;
      if (rowTemp < day.minTemperature) day.minTemperature = rowTemp;
      if (rowTemp > day.maxTemperature) day.maxTemperature = rowTemp;
      if (rowCloudBaseM > 0.0F && rowCloudBaseM < day.minCloudBaseMeters) day.minCloudBaseMeters = rowCloudBaseM;
      day.rainMm += rowRainMm;
    }

    day.hourlyCount = outIndex;
    if (windowCount == 0) {
      if (dayIndex > 0) {
        continue;
      }
      day.maxRainProbability = day.rainProbability;
      day.maxCloudCover = day.cloudCover;
      day.maxWindSpeedKmh = day.windSpeedKmh;
      day.maxWindGustKmh = day.windGustKmh;
      day.minCloudBaseMeters = day.cloudBaseMeters;
    } else if (day.minCloudBaseMeters == 99999.0F) {
      day.minCloudBaseMeters = day.cloudBaseMeters;
    }
    if (day.cloudCover == 0 && day.hourly[0].cloudCover > 0) {
      day.cloudCover = day.hourly[0].cloudCover;
    }

    applyFlightCalculations(day);
    days_[forecastDayCount_++] = day;
  }

  if (forecastDayCount_ == 0) {
    WeatherFlightData day = base;
    applyFlightCalculations(day);
    days_[forecastDayCount_++] = day;
  }
  data_ = days_[0];
}

float OpenWeatherClient::calculateCloudBase(float temp, float dewPoint) {
  const float spread = temp - dewPoint;
  if (spread <= 0.0F) {
    return 0.0F;
  }
  return spread * 125.0F;
}

float OpenWeatherClient::calculateDewPoint(float temp, uint8_t humidity) {
  const float rh = clampFloat(static_cast<float>(humidity), 1.0F, 100.0F);
  const float a = 17.62F;
  const float b = 243.12F;
  const float gamma = logf(rh / 100.0F) + (a * temp) / (b + temp);
  return (b * gamma) / (a - gamma);
}

int OpenWeatherClient::calculateFlightIndex(const WeatherFlightData& data) {
  int score = 100;
  const uint8_t rainRisk = data.maxRainProbability > data.rainProbability ? data.maxRainProbability : data.rainProbability;
  const uint8_t cloudRisk = data.maxCloudCover > data.cloudCover ? data.maxCloudCover : data.cloudCover;
  const float windRisk = data.maxWindSpeedKmh > data.windSpeedKmh ? data.maxWindSpeedKmh : data.windSpeedKmh;
  const float gustRisk = data.maxWindGustKmh > data.windGustKmh ? data.maxWindGustKmh : data.windGustKmh;

  if (data.windSpeedKmh < 5.0F) {
    score -= static_cast<int>((5.0F - data.windSpeedKmh) * 7.0F);
  } else if (windRisk > 22.0F) {
    score -= static_cast<int>((windRisk - 22.0F) * 5.0F);
  }

  if (windRisk > 32.0F) {
    score -= 18;
  }
  if (gustRisk > 30.0F) {
    score -= static_cast<int>((gustRisk - 30.0F) * 4.0F) + 15;
  }
  if (rainRisk > 30) {
    score -= static_cast<int>((rainRisk - 30) * 1.2F) + 18;
  }
  if (data.rainMm > 0.1F) {
    score -= 22;
  }
  if (data.cloudCover < 10) {
    score -= 6;
  } else if (cloudRisk > 70) {
    score -= static_cast<int>((cloudRisk - 70) * 0.6F);
  }
  if (data.cloudBaseMeters < 800.0F) {
    score -= static_cast<int>((800.0F - data.cloudBaseMeters) / 18.0F);
  }
  if (data.humidity > 85 && data.cloudBaseMeters < 900.0F) {
    score -= 20;
  }
  if (data.visibility > 0 && data.visibility < 5) {
    score -= 15;
  }

  return clampInt(score, 0, 100);
}

String OpenWeatherClient::getWindDirectionText(int degrees) {
  int normalized = degrees % 360;
  if (normalized < 0) normalized += 360;
  const int sector = static_cast<int>((normalized + 22) / 45) % 8;
  switch (sector) {
    case 0:
      return "N";
    case 1:
      return "NE";
    case 2:
      return "L";
    case 3:
      return "SE";
    case 4:
      return "S";
    case 5:
      return "SO";
    case 6:
      return "O";
    default:
      return "NO";
  }
}

void OpenWeatherClient::printWeatherDebug() const {
  Serial.printf("OpenWeather: temp=%.1fC vento=%.1f raj=%.1f dir=%s/%d chuva=%u%% base=%.0fm indice=%d status=%s alerta=%s\n",
                data_.temperature,
                data_.windSpeedKmh,
                data_.windGustKmh,
                data_.windDirectionText,
                data_.windDirectionDeg,
                static_cast<unsigned>(data_.rainProbability),
                data_.cloudBaseMeters,
                data_.flightIndex,
                data_.flightStatus,
                data_.alertMessage);
}

const char* OpenWeatherClient::statusText() const {
  switch (state_) {
    case OpenWeatherState::Idle:
      return "NAO ATUALIZADO";
    case OpenWeatherState::WaitingWifi:
      return "LIGANDO WIFI";
    case OpenWeatherState::Fetching:
      return "BUSCANDO PREVISAO";
    case OpenWeatherState::Ready:
      return forecastPartial_ ? "METEO ATUAL OK" : "PREVISAO ATUALIZADA";
    case OpenWeatherState::Error:
      return errorText_;
  }
  return "METEO DESCONHECIDO";
}

bool OpenWeatherClient::isConfigured() const {
  return normalizedApiKey().length() > 0;
}

bool OpenWeatherClient::validLocation(double lat, double lon) const {
  if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
    return false;
  }
  return fabs(lat) > 0.0001 || fabs(lon) > 0.0001;
}

void OpenWeatherClient::setError(const char* text) {
  copyText(errorText_, sizeof(errorText_), text);
  state_ = OpenWeatherState::Error;
  requestPending_ = false;
  hasData_ = false;
  forecastDayCount_ = 0;
  dataLocationKey_ = 0;
  forecastPartial_ = false;
  Serial.printf("OpenWeather: %s\n", errorText_);
}

bool OpenWeatherClient::httpGet(const String& url, String& payload, char* errorText, size_t errorTextSize) {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(OpenWeatherConfig::kHttpTimeoutMs);
  http.setReuse(false);
  if (!http.begin(client, url)) {
    copyText(errorText, errorTextSize, "HTTP BEGIN FALHOU");
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    char text[64];
    if (code == HTTP_CODE_UNAUTHORIZED) {
      snprintf(text, sizeof(text), "API KEY/PLANO 401");
    } else if (code == HTTP_CODE_TOO_MANY_REQUESTS) {
      snprintf(text, sizeof(text), "LIMITE API 429");
    } else if (code < 0) {
      snprintf(text, sizeof(text), "HTTP FALHA %d", code);
    } else {
      snprintf(text, sizeof(text), "HTTP ERRO %d", code);
    }
    copyText(errorText, errorTextSize, text);
    String body = http.getString();
    if (body.length() > 0) {
      body.replace("\n", " ");
      body.replace("\r", " ");
      if (body.length() > 160) {
        body = body.substring(0, 160);
      }
      Serial.printf("OpenWeather resposta: %s\n", body.c_str());
    }
    http.end();
    return false;
  }

  payload = http.getString();
  http.end();
  if (payload.length() == 0) {
    copyText(errorText, errorTextSize, "RESPOSTA VAZIA");
    return false;
  }
  return true;
}

void OpenWeatherClient::applyFlightCalculations(WeatherFlightData& data) {
  data.cloudBaseMeters = calculateCloudBase(data.temperature, data.dewPoint);
  if (data.minCloudBaseMeters <= 0.0F || data.cloudBaseMeters < data.minCloudBaseMeters) {
    data.minCloudBaseMeters = data.cloudBaseMeters;
  }
  data.cloudTopMeters = estimateCloudTopMeters(data);
  data.thermalStrengthMs = estimateThermalStrengthMs(data);
  classifyThermal(data.thermalClass, sizeof(data.thermalClass), data.thermalStrengthMs);

  uint32_t localStartUtc = data.forecastStartUtc;
  uint32_t localPeakUtc = data.forecastStartUtc;
  uint32_t localEndUtc = data.forecastEndUtc;
  chooseLocalFlightWindow(data, localStartUtc, localPeakUtc, localEndUtc);

  formatLocalHourMinute(data.flightStartTime, sizeof(data.flightStartTime), localStartUtc, data.timezoneOffsetSeconds);
  formatLocalHourMinute(data.flightPeakTime, sizeof(data.flightPeakTime), localPeakUtc, data.timezoneOffsetSeconds);
  formatLocalHourMinute(data.flightEndTime, sizeof(data.flightEndTime), localEndUtc, data.timezoneOffsetSeconds);
  snprintf(data.flightWindowText, sizeof(data.flightWindowText), "%s-%s", data.flightStartTime, data.flightEndTime);

  data.flightIndex = calculateFlightIndex(data);

  if (data.flightIndex >= 75) {
    copyText(data.flightStatus, sizeof(data.flightStatus), "BOM PARA VOO");
  } else if (data.flightIndex >= 45) {
    copyText(data.flightStatus, sizeof(data.flightStatus), "DA PARA VOAR");
  } else {
    copyText(data.flightStatus, sizeof(data.flightStatus), "RUIM PARA VOO");
  }

  if (data.flightIndex >= 85) {
    copyText(data.flightRating, sizeof(data.flightRating), "MUITO BOM");
  } else if (data.flightIndex >= 70) {
    copyText(data.flightRating, sizeof(data.flightRating), "BOM");
  } else if (data.flightIndex >= 50) {
    copyText(data.flightRating, sizeof(data.flightRating), "TECNICO");
  } else {
    copyText(data.flightRating, sizeof(data.flightRating), "RUIM");
  }

  if (data.flightIndex >= 80 && data.cloudBaseMeters >= 1200.0F && data.windSpeedKmh >= 8.0F && data.windSpeedKmh <= 24.0F &&
      data.maxRainProbability <= 20) {
    copyText(data.xcStatus, sizeof(data.xcStatus), "BOM PARA XC");
  } else if (data.flightIndex >= 65 && data.cloudBaseMeters >= 900.0F && data.maxRainProbability <= 30) {
    copyText(data.xcStatus, sizeof(data.xcStatus), "XC TECNICO");
  } else if (data.cloudBaseMeters < 800.0F || data.humidity > 88) {
    copyText(data.xcStatus, sizeof(data.xcStatus), "TETO BAIXO");
  } else if (data.windSpeedKmh < 6.0F) {
    copyText(data.xcStatus, sizeof(data.xcStatus), "VOO LOCAL");
  } else if (data.windGustKmh > 32.0F || data.windSpeedKmh > 28.0F) {
    copyText(data.xcStatus, sizeof(data.xcStatus), "XC ARRISCADO");
  } else {
    copyText(data.xcStatus, sizeof(data.xcStatus), "AVALIAR LOCAL");
  }

  if (data.windGustKmh > 35.0F) {
    copyText(data.alertMessage, sizeof(data.alertMessage), "Rajada forte. Avaliar decolagem.");
  } else if (data.windSpeedKmh > 28.0F) {
    copyText(data.alertMessage, sizeof(data.alertMessage), "Vento forte para voo livre.");
  } else if (data.rainProbability > 30 || data.rainMm > 0.1F) {
    copyText(data.alertMessage, sizeof(data.alertMessage), "Risco de chuva na janela.");
  } else if (data.humidity > 85 && data.cloudBaseMeters < 900.0F) {
    copyText(data.alertMessage, sizeof(data.alertMessage), "Umidade alta e teto baixo.");
  } else if (data.windSpeedKmh < 5.0F) {
    copyText(data.alertMessage, sizeof(data.alertMessage), "Vento fraco, pouco rendimento.");
  } else {
    copyText(data.alertMessage, sizeof(data.alertMessage), "Sem alerta critico.");
  }

  if (data.maxRainProbability > 45) {
    copyText(data.alertMessage2, sizeof(data.alertMessage2), "Chuva provavel na janela da previsao.");
  } else if (data.maxCloudCover > 80) {
    copyText(data.alertMessage2, sizeof(data.alertMessage2), "Muita nebulosidade, sol/termica limitado.");
  } else if (data.cloudBaseMeters > 1500.0F && data.cloudCover >= 15 && data.cloudCover <= 65) {
    copyText(data.alertMessage2, sizeof(data.alertMessage2), "Teto e nuvens favoraveis para deriva/XC.");
  } else {
    copyText(data.alertMessage2, sizeof(data.alertMessage2), "Sem alerta secundario.");
  }

  if (data.windSpeedKmh >= 8.0F && data.windSpeedKmh <= 22.0F && data.windGustKmh - data.windSpeedKmh <= 10.0F) {
    copyText(data.alertMessage3, sizeof(data.alertMessage3), "Vento dentro da faixa ideal do projeto.");
  } else if (data.windSpeedKmh < 5.0F) {
    copyText(data.alertMessage3, sizeof(data.alertMessage3), "Pouca deriva; XC pode depender de termica forte.");
  } else {
    copyText(data.alertMessage3, sizeof(data.alertMessage3), "Cheque rajadas e rotor no relevo.");
  }

  buildFlightSummary(data);
}
