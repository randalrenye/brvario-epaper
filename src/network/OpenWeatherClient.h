#pragma once

#include <Arduino.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class WifiManager;

static constexpr uint8_t kWeatherHourlyForecastCount = 4;
static constexpr uint8_t kWeatherForecastDayCount = 5;
static constexpr uint8_t kWeatherWindForecastCount = 16;

struct WeatherHourlyForecast {
  uint32_t timeUtc = 0;
  float temperature = 0.0F;
  float windSpeedKmh = 0.0F;
  int windDirectionDeg = 0;
  uint8_t rainProbability = 0;
  uint8_t cloudCover = 0;
  char condition[18] = {};
};

struct WeatherFlightData {
  float temperature = 0.0F;
  float minTemperature = 0.0F;
  float maxTemperature = 0.0F;
  float feelsLike = 0.0F;
  uint8_t humidity = 0;
  float pressure = 0.0F;
  float apiPressure = 0.0F;
  float localPressure = 0.0F;
  bool usingLocalPressure = false;
  float dewPoint = 0.0F;
  float windSpeedKmh = 0.0F;
  float windGustKmh = 0.0F;
  int windDirectionDeg = 0;
  char windDirectionText[4] = {};
  uint8_t cloudCover = 0;
  uint8_t rainProbability = 0;
  uint8_t maxRainProbability = 0;
  uint8_t maxCloudCover = 0;
  float rainMm = 0.0F;
  float uvi = 0.0F;
  bool uviAvailable = false;
  uint16_t visibility = 0;
  float cloudBaseMeters = 0.0F;
  float minCloudBaseMeters = 0.0F;
  float cloudTopMeters = 0.0F;
  float thermalStrengthMs = 0.0F;
  char thermalClass[16] = {};
  float maxWindSpeedKmh = 0.0F;
  float maxWindGustKmh = 0.0F;
  int flightIndex = 0;
  char flightRating[18] = {};
  char flightStatus[18] = {};
  char xcStatus[24] = {};
  char flightStartTime[8] = {};
  char flightPeakTime[8] = {};
  char flightEndTime[8] = {};
  char flightWindowText[22] = {};
  char flightSummary[96] = {};
  char alertMessage[72] = {};
  char alertMessage2[72] = {};
  char alertMessage3[72] = {};
  char condition[24] = {};
  char forecastDateText[18] = {};
  uint32_t currentUtc = 0;
  uint32_t sunriseUtc = 0;
  uint32_t sunsetUtc = 0;
  uint32_t forecastStartUtc = 0;
  uint32_t forecastEndUtc = 0;
  int32_t timezoneOffsetSeconds = 0;
  WeatherHourlyForecast hourly[kWeatherHourlyForecastCount] = {};
  uint8_t hourlyCount = 0;
  uint32_t updatedAtMs = 0;
};

enum class OpenWeatherState : uint8_t {
  Idle,
  WaitingWifi,
  Fetching,
  Ready,
  Error,
};

class OpenWeatherClient {
 public:
  void begin(WifiManager* wifi);
  void request(double lat, double lon, float localPressureHpa, uint32_t locationKey = 0);
  void invalidate();
  void update();

  bool fetchOpenWeatherData(double lat, double lon);
  void parseOpenWeatherJson(String payload);
  static float calculateCloudBase(float temp, float dewPoint);
  static float calculateDewPoint(float temp, uint8_t humidity);
  static int calculateFlightIndex(const WeatherFlightData& data);
  static String getWindDirectionText(int degrees);
  void printWeatherDebug() const;

  const WeatherFlightData& data(uint8_t dayIndex = 0) const;
  uint8_t forecastDayCount() const { return forecastDayCount_; }
  OpenWeatherState state() const { return state_; }
  const char* statusText() const;
  const char* errorText() const { return errorText_; }
  bool hasData() const { return hasData_; }
  bool dataMatches(uint32_t locationKey) const { return hasData_ && dataLocationKey_ == locationKey; }
  uint32_t dataLocationKey() const { return dataLocationKey_; }
  uint32_t requestedLocationKey() const { return pendingLocationKey_; }
  double requestedLatitude() const { return pendingLat_; }
  double requestedLongitude() const { return pendingLon_; }
  uint8_t windForecastCount() const { return windForecastCount_; }
  const WeatherHourlyForecast& windForecast(uint8_t index) const {
    return windForecast_[index < windForecastCount_ ? index : 0];
  }
  bool busy() const {
    return state_ == OpenWeatherState::WaitingWifi || state_ == OpenWeatherState::Fetching || fetchTaskRunning_ || fetchResultReady_;
  }
  bool isConfigured() const;

 private:
  WifiManager* wifi_ = nullptr;
  WeatherFlightData data_ = {};
  WeatherFlightData days_[kWeatherForecastDayCount] = {};
  WeatherHourlyForecast windForecast_[kWeatherWindForecastCount] = {};
  uint8_t forecastDayCount_ = 0;
  uint8_t windForecastCount_ = 0;
  OpenWeatherState state_ = OpenWeatherState::Idle;
  bool hasData_ = false;
  bool requestPending_ = false;
  bool forecastPartial_ = false;
  double pendingLat_ = 0.0;
  double pendingLon_ = 0.0;
  float pendingLocalPressureHpa_ = 0.0F;
  uint32_t pendingLocationKey_ = 0;
  uint32_t requestGeneration_ = 0;
  double fetchLat_ = 0.0;
  double fetchLon_ = 0.0;
  float fetchLocalPressureHpa_ = 0.0F;
  uint32_t fetchLocationKey_ = 0;
  uint32_t fetchGeneration_ = 0;
  double dataLat_ = 0.0;
  double dataLon_ = 0.0;
  uint32_t dataLocationKey_ = 0;
  uint32_t waitStartedMs_ = 0;
  char errorText_[80] = {};
  TaskHandle_t fetchTaskHandle_ = nullptr;
  volatile bool fetchTaskRunning_ = false;
  volatile bool fetchResultReady_ = false;
  bool fetchResultSuccess_ = false;
  bool fetchForecastAvailable_ = false;
  String fetchCurrentPayload_;
  String fetchForecastPayload_;
  char fetchErrorText_[80] = {};

  bool validLocation(double lat, double lon) const;
  void setError(const char* text);
  void buildCurrentOnlyForecast();
  bool fetchOpenWeatherPayloads(double lat,
                                double lon,
                                String& currentPayload,
                                String& forecastPayload,
                                bool& forecastAvailable,
                                char* errorText,
                                size_t errorTextSize);
  bool httpGet(const String& url, String& payload, char* errorText, size_t errorTextSize);
  void parseForecastJson(String payload);
  void applyFlightCalculations(WeatherFlightData& data);
  bool startFetchTask();
  static void fetchTaskThunk(void* context);
  void runFetchTask();
  void finishFetchResult();
};
