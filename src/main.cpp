#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <time.h>

#include "audio/VarioBuzzer.h"
#include "assets/BrvarioBootLogo.h"
#include "ble/TracklogBleService.h"
#include "clock/ClockManager.h"
#include "config/AppConfig.h"
#include "config/PilotProfileConfig.h"
#include "config/ThermalAssistConfig.h"
#include "data/VarioData.h"
#include "display/EpdDisplay.h"
#include "network/FirmwareUpdater.h"
#include "network/FlightSiteCatalogUpdater.h"
#include "network/MapDownloadManager.h"
#include "network/OpenWeatherClient.h"
#include "network/WeatherWindCache.h"
#include "network/WifiManager.h"
#include "navigation/FlightMetrics.h"
#include "navigation/ThermalAssistant.h"
#include "navigation/WindEstimator.h"
#include "sensors/BatteryMonitor.h"
#include "sensors/Bmp280Barometer.h"
#include "sensors/GpsManager.h"
#include "storage/StorageManager.h"
#include "system/I2CBusLock.h"
#include "TouchManager.h"
#include "tracklog/FlightRecorder.h"
#include "ui/MainScreen.h"
#include "utilities.h"
#include "weather/FlightSiteCatalog.h"
#include "weather/WeatherLocationManager.h"

#ifndef BOARD_HAS_PSRAM
#error "BOARD_HAS_PSRAM must be enabled for the LilyGo EPD47 framebuffer."
#endif

EpdDisplay display;
GpsManager gpsManager;
BatteryMonitor batteryMonitor;
Bmp280Barometer barometer;
VarioBuzzer varioBuzzer;
WifiManager wifiManager;
FirmwareUpdater firmwareUpdater;
FlightSiteCatalogUpdater flightSiteCatalogUpdater;
MapDownloadManager mapDownloadManager;
OpenWeatherClient openWeatherClient;
WeatherWindCache weatherWindCache;
WeatherLocationManager weatherLocationManager;
WindEstimator windEstimator;
ThermalAssistant thermalAssistant;
FlightMetrics flightMetrics;
ClockManager clockManager;
PilotProfileConfig pilotProfile;
ThermalAssistConfig thermalAssistConfig;
FlightRecorder flightRecorder;
TracklogBleService tracklogBle;
StorageManager storageManager;
VarioData varioData;
MainScreen screen(display);
TouchManager touchManager;
Preferences appPreferences;

uint32_t lastUiUpdateMs = 0;
uint32_t lastTelemetryMs = 0;
uint32_t lastFullTelemetryMs = 0;
uint32_t lastConfigTelemetryMs = 0;
uint32_t lastWindNavigationMs = 0;
uint32_t lastThermalNavigationMs = 0;
uint32_t thermalGpsLostStartedMs = 0;
uint32_t lastUserActivityMs = 0;
uint32_t lastMotionActivityMs = 0;
uint32_t lastWifiActivityMs = 0;
bool touchReady = false;
bool gpsFixWasActive = false;
uint32_t gpsFixLostMs = 0;
bool audioUserEnabled = true;
bool configAudioMuted = false;
bool preferencesReady = false;
uint32_t lastNtpAttemptMs = 0;
uint32_t lastNtpReadMs = 0;
bool ntpConfigured = false;
bool tracklogStorageReady = false;
uint32_t lastWeatherWindCacheUtc = 0;
bool wifiAutoWeatherConnectionActive = false;
bool wifiAutoWeatherRequestSent = false;
bool wifiBootConnectPending = false;
uint32_t lastWifiAutoWeatherAttemptMs = 0;
uint32_t lastTracklogArchiveRefreshMs = 0;
uint32_t idlePowerWarningStartedMs = 0;
uint32_t lastIdlePowerWarningAlarmMs = 0;
bool idlePowerWarningActive = false;
uint32_t criticalBatteryStartedMs = 0;
bool criticalBatterySleepTriggered = false;
volatile bool realtimeVarioAudioEnabled = false;
uint32_t lastRuntimeHealthMs = 0;

namespace {

static constexpr uint32_t kTelemetryLogIntervalMs = 2500UL;
static constexpr uint32_t kRuntimeHealthIntervalMs = 30000UL;
static constexpr uint32_t kFullTelemetryIntervalMs = 50UL;
static constexpr uint32_t kConfigTelemetrySlowIntervalMs = 1000UL;
static constexpr uint32_t kCpuFrequencyMhz = 160UL;
static constexpr uint32_t kMapCpuFrequencyMhz = 240UL;
static constexpr bool kSafeBootRadioRecoveryMode = false;
static constexpr bool kKeepFullTelemetryWhileRecording = true;
static constexpr uint32_t kWifiAutoOffMs = 60UL * 1000UL;
static constexpr uint32_t kIdlePowerWarningMs = 25UL * 60UL * 1000UL;
static constexpr uint32_t kIdlePowerShutdownGraceMs = 5UL * 60UL * 1000UL;
static constexpr uint32_t kIdlePowerAlarmRepeatMs = 15UL * 1000UL;
static constexpr uint32_t kAutoSleepTouchGraceMs = 5000UL;
static constexpr uint32_t kGpsReconnectSoundMinLostMs = 8000UL;
static constexpr uint8_t kAutoSleepMotionMinSatellites = 4;
static constexpr float kAutoSleepMotionMaxHdop = 8.0F;
static constexpr float kAutoSleepMotionSpeedKmh = 2.0F;
static constexpr float kAutoSleepMotionVarioMs = 0.70F;
static constexpr uint8_t kCriticalBatterySleepPercent = 2;
static constexpr float kCriticalBatterySleepVoltage = 3.32F;
static constexpr uint32_t kCriticalBatteryConfirmMs = 15000UL;
static constexpr int32_t kSleepLogoFooterTextY = 475;
static constexpr uint64_t kWakeButtonMask = 1ULL << GPIO_NUM_21;
static constexpr long kSaoPauloUtcOffsetSeconds = -3L * 3600L;
static constexpr uint32_t kNtpRetryIntervalMs = 60000UL;
static constexpr uint32_t kAutoWeatherRetryMs = 3000UL;
static constexpr uint32_t kTracklogArchiveRefreshMs = 30000UL;
static constexpr uint32_t kThermalTurnWindowMs = 30000UL;
static constexpr uint32_t kThermalGpsLossGraceMs = 30000UL;
static constexpr float kThermalMinSpeedKmh = 4.0F;
static constexpr float kThermalMaxSpeedKmh = 55.0F;
static constexpr float kThermalTurnStartDeg = 90.0F;
static constexpr float kThermalTurnEndDeg = 30.0F;
static constexpr float kThermalLiftStartMinMs = -0.50F;
static constexpr float kThermalLiftEndMinMs = -0.50F;
static constexpr const char* kPrefsNamespace = "brvario";
static constexpr const char* kAudioEnabledKey = "audio_on";

float normalizeDeg(float value) {
  while (value < 0.0F) value += 360.0F;
  while (value >= 360.0F) value -= 360.0F;
  return value;
}

const char* resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "POWERON";
    case ESP_RST_EXT:
      return "EXTERNO";
    case ESP_RST_SW:
      return "SOFTWARE";
    case ESP_RST_PANIC:
      return "PANIC";
    case ESP_RST_INT_WDT:
      return "INT_WDT";
    case ESP_RST_TASK_WDT:
      return "TASK_WDT";
    case ESP_RST_WDT:
      return "WDT";
    case ESP_RST_DEEPSLEEP:
      return "DEEP_SLEEP";
    case ESP_RST_BROWNOUT:
      return "BROWNOUT";
    case ESP_RST_SDIO:
      return "SDIO";
    case ESP_RST_UNKNOWN:
    default:
      return "DESCONHECIDO";
  }
}

float shortestAngleDelta(float fromDeg, float toDeg) {
  float delta = normalizeDeg(toDeg) - normalizeDeg(fromDeg);
  if (delta > 180.0F) delta -= 360.0F;
  if (delta < -180.0F) delta += 360.0F;
  return delta;
}

bool shouldPlayTouchFeedback(TouchAction action) {
  switch (action) {
    case TouchAction::None:
    case TouchAction::PowerOff:
    case TouchAction::AudioPitchDown:
    case TouchAction::AudioPitchUp:
    case TouchAction::AudioVolumeSet:
    case TouchAction::AudioVoiceToggle:
      return false;
    default:
      return true;
  }
}

void setCpuFrequencyIfNeeded(uint32_t targetMhz) {
  if (getCpuFrequencyMhz() == targetMhz) {
    return;
  }
  setCpuFrequencyMhz(targetMhz);
  Serial.printf("CPU ajustada para %lu MHz.\n", static_cast<unsigned long>(targetMhz));
}

void applyCpuMode() {
  setCpuFrequencyIfNeeded(screen.isFlightDisplay() ? kMapCpuFrequencyMhz : kCpuFrequencyMhz);
}

const char* touchActionName(TouchAction action) {
  switch (action) {
    case TouchAction::Back:
      return "VOLTAR";
    case TouchAction::OpenSettings:
      return "CONFIGURACAO";
    case TouchAction::NextPage:
      return "MAPA";
    case TouchAction::ToggleTracklog:
      return "TRACKLOG";
    case TouchAction::TracklogBleCancel:
      return "BLE CANCELAR";
    case TouchAction::ToggleAudio:
      return "AUDIO";
    case TouchAction::Home:
      return "INICIO";
    case TouchAction::PowerRequest:
      return "DESLIGAR?";
    case TouchAction::PowerHold:
      return "DESLIGAR SEGURE";
    case TouchAction::PowerOff:
      return "DESLIGAR";
    case TouchAction::PowerConfirmYes:
      return "DESLIGAR SIM";
    case TouchAction::PowerConfirmNo:
      return "DESLIGAR NAO";
    case TouchAction::CenterGauge:
      return "VARIO";
    case TouchAction::OpenAudioEditor:
      return "EDITOR AUDIO";
    case TouchAction::OpenWifiSettings:
      return "WIFI";
    case TouchAction::OpenFirmwareUpdate:
      return "ATUALIZACAO";
    case TouchAction::OpenWeatherStation:
      return "ESTACAO METEOROLOGICA";
    case TouchAction::OpenPilotProfile:
      return "DADOS PILOTO";
    case TouchAction::OpenThermalCycleBeta:
      return "CICLO TERMAL BETA";
    case TouchAction::OpenDashboardLayout:
      return "PERSONALIZAR TELA INICIAL";
    case TouchAction::OpenThermalAssistSettings:
      return "ASSISTENTE TERMICA";
    case TouchAction::OpenDeviceInfo:
      return "INFORMACOES";
    case TouchAction::OpenSystemStatus:
      return "STATUS SISTEMA";
    case TouchAction::OpenStorageManager:
      return "MEMORIA MAPAS";
    case TouchAction::OpenAdvancedSystem:
      return "SISTEMA AVANCADO";
    case TouchAction::OpenMapDownload:
      return "DOWNLOAD MAPAS";
    case TouchAction::StorageRefresh:
      return "SD ATUALIZAR";
    case TouchAction::StorageClearMaps:
      return "SD LIMPAR MAPAS";
    case TouchAction::AdvancedRecoverDisplay:
      return "AVANCADO RECUPERAR TELA";
    case TouchAction::AdvancedMoveIgcToSd:
      return "AVANCADO IGC PARA SD";
    case TouchAction::AdvancedClearWifi:
      return "AVANCADO LIMPAR WIFI";
    case TouchAction::AdvancedResetSettings:
      return "AVANCADO PADRAO";
    case TouchAction::AdvancedClearWeatherCache:
      return "AVANCADO LIMPAR METEO";
    case TouchAction::AdvancedFormatSystem:
      return "AVANCADO FORMATAR";
    case TouchAction::AdvancedConfirmYes:
      return "AVANCADO SIM";
    case TouchAction::AdvancedConfirmNo:
      return "AVANCADO NAO";
    case TouchAction::MapDownloadSelect0:
    case TouchAction::MapDownloadSelect1:
    case TouchAction::MapDownloadSelect2:
      return "MAPA REGIAO";
    case TouchAction::MapDownloadStart:
      return "MAPA BAIXAR";
    case TouchAction::MapDownloadCancel:
      return "MAPA CANCELAR";
    case TouchAction::MapZoomIn:
      return "MAPA ZOOM +";
    case TouchAction::MapZoomOut:
      return "MAPA ZOOM -";
    case TouchAction::MapPanUp:
      return "MAPA PAN CIMA";
    case TouchAction::MapPanDown:
      return "MAPA PAN BAIXO";
    case TouchAction::MapPanLeft:
      return "MAPA PAN ESQ";
    case TouchAction::MapPanRight:
      return "MAPA PAN DIR";
    case TouchAction::DashboardLayoutMove:
      return "MOVER WIDGET";
    case TouchAction::DashboardLayoutSave:
      return "LAYOUT SALVAR";
    case TouchAction::DashboardLayoutReset:
      return "LAYOUT PADRAO";
    case TouchAction::AudioSave:
      return "AUDIO SALVAR";
    case TouchAction::AudioReset:
      return "AUDIO PADRAO";
    case TouchAction::AudioResponseDown:
      return "AUDIO RESPOSTA -";
    case TouchAction::AudioResponseUp:
      return "AUDIO RESPOSTA +";
    case TouchAction::AudioPitchDown:
      return "AUDIO TOM -";
    case TouchAction::AudioPitchUp:
      return "AUDIO TOM +";
    case TouchAction::AudioVolumeSet:
      return "AUDIO VOLUME";
    case TouchAction::AudioVoiceToggle:
      return "AUDIO VOZ";
    case TouchAction::ThermalModePilot:
      return "TERMICA PILOTO CENTRO";
    case TouchAction::ThermalModeThermal:
      return "TERMICA CENTRO";
    case TouchAction::ThermalInfo:
      return "TERMICA INFO";
    case TouchAction::ThermalInfoUp:
      return "TERMICA INFO ACIMA";
    case TouchAction::ThermalInfoDown:
      return "TERMICA INFO ABAIXO";
    case TouchAction::ThermalCycleInfo:
      return "CICLO TERMAL INFO";
    case TouchAction::ThermalCycleInfoUp:
      return "CICLO TERMAL INFO ACIMA";
    case TouchAction::ThermalCycleInfoDown:
      return "CICLO TERMAL INFO ABAIXO";
    case TouchAction::WeatherPrevDay:
      return "METEO DIA ANTERIOR";
    case TouchAction::WeatherNextDay:
      return "METEO PROXIMO DIA";
    case TouchAction::WeatherInfo:
      return "METEO INFO";
    case TouchAction::WeatherInfoUp:
      return "METEO INFO SOBE";
    case TouchAction::WeatherInfoDown:
      return "METEO INFO DESCE";
    case TouchAction::OpenWeatherLocation:
      return "METEO LOCAL";
    case TouchAction::WeatherLocationAction:
      return "METEO SELECIONAR LOCAL";
    default:
      return "NENHUM";
  }
}

void drawPackedLogo4bpp(int32_t x, int32_t y) {
  uint8_t* fb = display.framebuffer();
  if (!fb) return;

  for (int32_t py = 0; py < BrvarioBootLogo::kHeight; ++py) {
    const int32_t screenY = y + py;
    if (screenY < 0 || screenY >= EPD_HEIGHT) continue;

    for (int32_t px = 0; px < BrvarioBootLogo::kWidth; ++px) {
      const int32_t screenX = x + px;
      if (screenX < 0 || screenX >= EPD_WIDTH) continue;

      const size_t logoIndex = static_cast<size_t>(py) * ((BrvarioBootLogo::kWidth + 1) / 2) + static_cast<size_t>(px / 2);
      const uint8_t packed = BrvarioBootLogo::kPixels[logoIndex];
      const uint8_t value = (px & 1) ? ((packed >> 4) & 0x0F) : (packed & 0x0F);
      if (value == 0x0F) continue;

      uint8_t& dst = fb[static_cast<size_t>(screenY) * EPD_WIDTH / 2 + screenX / 2];
      if (screenX & 1) {
        dst = (dst & 0x0F) | (value << 4);
      } else {
        dst = (dst & 0xF0) | value;
      }
    }
  }
}

void drawPackedLogo4bppScaled(int32_t x, int32_t y, int32_t targetW, int32_t targetH) {
  uint8_t* fb = display.framebuffer();
  if (!fb || targetW <= 0 || targetH <= 0) return;

  const int32_t stride = (BrvarioBootLogo::kWidth + 1) / 2;
  for (int32_t dy = 0; dy < targetH; ++dy) {
    const int32_t screenY = y + dy;
    if (screenY < 0 || screenY >= EPD_HEIGHT) continue;
    const int32_t sy = (dy * BrvarioBootLogo::kHeight) / targetH;

    for (int32_t dx = 0; dx < targetW; ++dx) {
      const int32_t screenX = x + dx;
      if (screenX < 0 || screenX >= EPD_WIDTH) continue;
      const int32_t sx = (dx * BrvarioBootLogo::kWidth) / targetW;

      const size_t logoIndex = static_cast<size_t>(sy) * stride + static_cast<size_t>(sx / 2);
      const uint8_t packed = BrvarioBootLogo::kPixels[logoIndex];
      const uint8_t value = (sx & 1) ? ((packed >> 4) & 0x0F) : (packed & 0x0F);
      if (value == 0x0F) continue;

      uint8_t& dst = fb[static_cast<size_t>(screenY) * EPD_WIDTH / 2 + screenX / 2];
      if (screenX & 1) {
        dst = (dst & 0x0F) | (value << 4);
      } else {
        dst = (dst & 0xF0) | value;
      }
    }
  }
}

void renderBrandScreen() {
  display.clearBuffer(AppConfig::kWhite);
  const int32_t logoH = 500;
  const int32_t logoW = (BrvarioBootLogo::kWidth * logoH) / BrvarioBootLogo::kHeight;
  drawPackedLogo4bppScaled((EPD_WIDTH - logoW) / 2, 20, logoW, logoH);
  display.fullRefresh();
}

void renderSleepLogoScreen();
void enterWeatherDeepSleep();
void enterCriticalBatteryDeepSleep();
void enterDeepSleepAfterWeatherScreen();

bool isLeapYear(int year) {
  return ((year % 4) == 0 && (year % 100) != 0) || ((year % 400) == 0);
}

uint8_t daysInMonth(int year, int month) {
  static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && isLeapYear(year)) {
    return 29;
  }
  return days[month - 1];
}

bool utcEpochFromTm(const struct tm& timeInfo, uint32_t& epoch) {
  const int year = timeInfo.tm_year + 1900;
  const int month = timeInfo.tm_mon + 1;
  if (year < 2024 || year > 2099 || month < 1 || month > 12 || timeInfo.tm_mday < 1 ||
      timeInfo.tm_mday > daysInMonth(year, month)) {
    return false;
  }

  uint32_t days = 0;
  for (int y = 1970; y < year; ++y) {
    days += isLeapYear(y) ? 366UL : 365UL;
  }
  for (int m = 1; m < month; ++m) {
    days += daysInMonth(year, m);
  }
  days += static_cast<uint32_t>(timeInfo.tm_mday - 1);
  epoch = days * 86400UL +
          static_cast<uint32_t>(timeInfo.tm_hour) * 3600UL +
          static_cast<uint32_t>(timeInfo.tm_min) * 60UL +
          static_cast<uint32_t>(timeInfo.tm_sec);
  return true;
}

uint32_t currentUtcEpoch() {
  struct tm gpsUtc = {};
  uint32_t epoch = 0;
  if (gpsManager.getUtcDateTime(gpsUtc) && utcEpochFromTm(gpsUtc, epoch)) {
    return epoch;
  }

  const time_t systemNow = time(nullptr);
  if (systemNow > 1700000000) {
    return static_cast<uint32_t>(systemNow);
  }

  if (openWeatherClient.hasData()) {
    return openWeatherClient.data(0).currentUtc;
  }

  return 0;
}

void updateWeatherWindCacheFromApi() {
  if (weatherLocationManager.source() != WeatherLocationSource::GpsCurrent ||
      !openWeatherClient.dataMatches(weatherLocationManager.locationKey())) {
    return;
  }
  if (!openWeatherClient.hasData() || openWeatherClient.forecastDayCount() == 0) {
    return;
  }

  const WeatherFlightData& weather = openWeatherClient.data(0);
  if (weather.currentUtc == 0 || weather.currentUtc == lastWeatherWindCacheUtc) {
    return;
  }

  lastWeatherWindCacheUtc = weather.currentUtc;
  if (weatherWindCache.updateFromWeather(openWeatherClient)) {
    Serial.printf("Vento meteo 3h salvo: %.1f km/h %.0f deg ate %lu UTC.\n",
                  static_cast<double>(weatherWindCache.speedKmh()),
                  static_cast<double>(weatherWindCache.directionDeg()),
                  static_cast<unsigned long>(weatherWindCache.validUntilUtc()));
  } else {
    Serial.println("Vento meteo 3h indisponivel para a janela selecionada.");
  }
}

void requestWeatherWhenWifiConnected(uint32_t now) {
  if (!wifiManager.isConnected()) {
    wifiAutoWeatherConnectionActive = false;
    wifiAutoWeatherRequestSent = false;
    return;
  }

  if (!wifiAutoWeatherConnectionActive) {
    wifiAutoWeatherConnectionActive = true;
    wifiAutoWeatherRequestSent = false;
    lastWifiAutoWeatherAttemptMs = 0;
  }

  if (!openWeatherClient.isConfigured()) {
    return;
  }

  if (lastWifiAutoWeatherAttemptMs != 0 && now - lastWifiAutoWeatherAttemptMs < kAutoWeatherRetryMs) {
    return;
  }
  lastWifiAutoWeatherAttemptMs = now;

  gpsManager.applyTo(varioData);
  weatherLocationManager.updateGpsLocation(varioData.latitudeDeg, varioData.longitudeDeg, varioData.gpsFix);
  const uint32_t locationKey = weatherLocationManager.locationKey();
  if (wifiAutoWeatherRequestSent &&
      !openWeatherClient.dataMatches(locationKey) &&
      !(openWeatherClient.busy() && openWeatherClient.requestedLocationKey() == locationKey)) {
    wifiAutoWeatherRequestSent = false;
  }
  if ((openWeatherClient.busy() && openWeatherClient.requestedLocationKey() == locationKey) ||
      openWeatherClient.dataMatches(locationKey)) {
    wifiAutoWeatherRequestSent = true;
    return;
  }
  if (openWeatherClient.busy() || !weatherLocationManager.hasValidLocation()) {
    return;
  }

  const float localPressure =
      weatherLocationManager.source() == WeatherLocationSource::GpsCurrent ? varioData.pressureHpa : 0.0F;
  Serial.printf("OpenWeather: atualizando local %s (%.6f, %.6f).\n",
                weatherLocationManager.displayName(),
                weatherLocationManager.latitude(),
                weatherLocationManager.longitude());
  openWeatherClient.request(weatherLocationManager.latitude(), weatherLocationManager.longitude(), localPressure, locationKey);
  wifiAutoWeatherRequestSent = true;
}

const char* windQualityName(WindQuality quality) {
  switch (quality) {
    case WindQuality::High:
      return "HIGH";
    case WindQuality::Medium:
      return "MEDIUM";
    case WindQuality::Low:
      return "LOW";
    case WindQuality::None:
    default:
      return "NONE";
  }
}

void applyWindEstimate() {
  const uint32_t navMs = gpsManager.lastNavigationMs();
  if (varioData.gpsFix && navMs != 0 && navMs != lastWindNavigationMs) {
    windEstimator.update(varioData.courseDeg,
                         varioData.groundSpeedKmh,
                         varioData.latitudeDeg,
                         varioData.longitudeDeg,
                         navMs);
    lastWindNavigationMs = navMs;
  }

  if (windEstimator.hasWind()) {
    varioData.windSpeedKmh = windEstimator.getWindSpeed();
    varioData.windDirectionDeg = windEstimator.getWindDirectionTo();
    varioData.windDirectionToDeg = varioData.windDirectionDeg;
    varioData.windQuality = windEstimator.getQuality();
  } else if (weatherWindCache.hasValid(currentUtcEpoch())) {
    varioData.windSpeedKmh = weatherWindCache.speedKmh();
    varioData.windDirectionDeg = weatherWindCache.directionDeg();
    varioData.windDirectionToDeg = varioData.windDirectionDeg;
    varioData.windQuality = WindQuality::Low;
  } else {
    varioData.windSpeedKmh = 0.0F;
    varioData.windDirectionDeg = 0.0F;
    varioData.windDirectionToDeg = 180.0F;
    varioData.windQuality = WindQuality::None;
  }
}

bool detectCirclingFromGps(float courseDeg, float groundSpeedKmh, float varioMs, uint32_t navMs) {
  static bool courseValid = false;
  static float previousCourseDeg = 0.0F;
  static float turnAccumDeg = 0.0F;
  static float filteredLiftMs = 0.0F;
  static uint32_t windowStartMs = 0;
  static bool circling = false;

  if (!varioData.gpsFix || groundSpeedKmh < kThermalMinSpeedKmh || groundSpeedKmh > kThermalMaxSpeedKmh) {
    courseValid = false;
    turnAccumDeg = 0.0F;
    filteredLiftMs = 0.0F;
    windowStartMs = navMs;
    circling = false;
    return false;
  }

  if (!courseValid) {
    previousCourseDeg = normalizeDeg(courseDeg);
    windowStartMs = navMs;
    courseValid = true;
    filteredLiftMs = varioMs;
    circling = false;
    return false;
  }

  if (navMs - windowStartMs > kThermalTurnWindowMs) {
    turnAccumDeg *= 0.45F;
    windowStartMs = navMs;
  }

  const float delta = fabsf(shortestAngleDelta(previousCourseDeg, courseDeg));
  previousCourseDeg = normalizeDeg(courseDeg);
  filteredLiftMs = filteredLiftMs * 0.90F + varioMs * 0.10F;
  if (delta > 0.5F && delta < 75.0F) {
    turnAccumDeg += delta;
  }

  if (turnAccumDeg >= kThermalTurnStartDeg && filteredLiftMs >= kThermalLiftStartMinMs) {
    circling = true;
  } else if (turnAccumDeg < kThermalTurnEndDeg && filteredLiftMs < kThermalLiftEndMinMs) {
    circling = false;
  }

  return circling;
}

void updateThermalAssistant() {
  varioData.thermalVisualMode = thermalAssistConfig.visualMode();
  const uint32_t navMs = gpsManager.lastNavigationMs();
  if (!varioData.gpsFix || navMs == 0) {
    const uint32_t now = millis();
    if (thermalGpsLostStartedMs == 0) {
      thermalGpsLostStartedMs = now;
    }
    if (now - thermalGpsLostStartedMs >= kThermalGpsLossGraceMs) {
      thermalAssistant.reset(varioData);
      flightMetrics.reset(varioData);
      lastThermalNavigationMs = 0;
    }
    return;
  }

  thermalGpsLostStartedMs = 0;
  if (navMs == lastThermalNavigationMs) {
    return;
  }

  const bool isCircling = detectCirclingFromGps(varioData.courseDeg, varioData.groundSpeedKmh, varioData.varioMs, navMs);
  thermalAssistant.update(varioData,
                          varioData.latitudeDeg,
                          varioData.longitudeDeg,
                          varioData.varioMs,
                          varioData.courseDeg,
                          varioData.windSpeedKmh / 3.6F,
                          varioData.windDirectionDeg,
                          isCircling);
  flightMetrics.update(varioData, navMs, isCircling);
  lastThermalNavigationMs = navMs;
}

void updateInternetClock() {
  const uint32_t now = millis();
  if (!wifiManager.isConnected()) {
    ntpConfigured = false;
    return;
  }

  if (!ntpConfigured || now - lastNtpAttemptMs >= kNtpRetryIntervalMs) {
    configTime(kSaoPauloUtcOffsetSeconds, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    ntpConfigured = true;
    lastNtpAttemptMs = now;
    Serial.println("Relogio: solicitando sincronizacao NTP.");
  }

  if (now - lastNtpReadMs < 1000UL) {
    return;
  }
  lastNtpReadMs = now;

  struct tm timeInfo;
  if (getLocalTime(&timeInfo, 5)) {
    clockManager.updateFromNtpLocal(timeInfo);
  }
}

void updateGpsClock() {
  struct tm gpsUtc = {};
  uint32_t utcSeconds = 0;
  if (gpsManager.getUtcDateTime(gpsUtc)) {
    clockManager.updateFromGpsUtc(gpsUtc);
  } else if (gpsManager.getUtcTimeOfDay(utcSeconds)) {
    clockManager.updateFromGpsUtcSeconds(utcSeconds);
  }
}

void updateClockSources() {
  updateInternetClock();
  updateGpsClock();
}

void applyClock(VarioData& data) {
  clockManager.applyTo(data);
}

void updateBatteryState() {
  batteryMonitor.update();
  batteryMonitor.applyTo(varioData);
}

void applyRadioStateToData() {
  varioData.wifiEnabled = wifiManager.isConnected();
  varioData.bluetoothActive = tracklogBle.enabled() || tracklogBle.connected();
  varioData.bluetoothConnected = tracklogBle.connected();
}

void applyAudioState() {
  configAudioMuted = !screen.isFlightDisplay() || screen.startupLocked();
  varioData.audioEnabled = audioUserEnabled && !configAudioMuted;
  realtimeVarioAudioEnabled = varioData.audioEnabled;
  if (!varioData.audioEnabled) {
    varioBuzzer.stop();
  }
}

void publishRealtimeVarioAudio(float varioMs, bool valid, void*) {
  varioBuzzer.update(varioMs, realtimeVarioAudioEnabled && valid);
}

void updateVarioAudio() {
  if (barometer.realtimePollingActive()) {
    return;
  }
  const bool enabled = !screen.startupLocked() && varioData.audioEnabled && barometer.audioDataValid();
  varioBuzzer.update(barometer.audioVarioMs(), enabled);
}

void updateFlightRecorder() {
  const uint32_t now = millis();
  if (!flightRecorder.recording() && now - lastTracklogArchiveRefreshMs >= kTracklogArchiveRefreshMs) {
    storageManager.ensureMounted();
    flightRecorder.attachArchiveStorage(storageManager.filesystem(), "/brvario/igc");
    lastTracklogArchiveRefreshMs = now;
  }
  struct tm gpsUtc = {};
  const bool hasGpsUtc = gpsManager.getUtcDateTime(gpsUtc);
  flightRecorder.update(varioData, gpsUtc, hasGpsUtc);
}

void handleGpsConnectedSound();
void handleTakeoffSound();

bool fullTelemetryActive() {
  return screen.isFlightDisplay() || (kKeepFullTelemetryWhileRecording && flightRecorder.recording());
}

bool backgroundTakeoffMonitorActive() {
  return !screen.isFlightDisplay() && !flightRecorder.recording();
}

void updateTelemetryPipeline(bool includeFlightRecorder) {
  updateClockSources();
  if (!barometer.realtimePollingActive()) {
    barometer.poll();
  }
  updateBatteryState();
  gpsManager.applyTo(varioData);
  applyWindEstimate();
  barometer.applyTo(varioData);
  updateThermalAssistant();
  if (includeFlightRecorder || backgroundTakeoffMonitorActive()) {
    updateFlightRecorder();
  }
  applyClock(varioData);
  applyRadioStateToData();
  applyAudioState();
  handleGpsConnectedSound();
  handleTakeoffSound();
}

void renderSleepLogoScreen() {
  display.clearBuffer(AppConfig::kWhite);
  drawPackedLogo4bpp((EPD_WIDTH - BrvarioBootLogo::kWidth) / 2, 42);
  display.drawSmallTextBoldAligned("E-PAPER", EPD_WIDTH / 2, kSleepLogoFooterTextY, 7, AppConfig::kBlack, EpdDisplay::Align::Center);
  display.fullRefresh();
}

void renderSafeBlankSleepScreen() {
  display.clearBuffer(AppConfig::kWhite);
  display.fullRefresh();
}

void enterDeepSleepAfterWeatherScreen() {
  gpsManager.end();
  barometer.end();
  wifiManager.end();
  tracklogBle.end();
  if (touchReady) {
    touchManager.sleep();
    touchReady = false;
  } else {
    Wire.end();
    pinMode(BOARD_SDA, OPEN_DRAIN);
    pinMode(BOARD_SCL, OPEN_DRAIN);
    pinMode(TOUCH_INT, OPEN_DRAIN);
  }

  epd_poweroff_all();
  if (preferencesReady) {
    appPreferences.end();
    preferencesReady = false;
  }
  Serial.flush();
  Serial.end();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
  esp_sleep_enable_ext1_wakeup(kWakeButtonMask, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();
}

void enterWeatherDeepSleep() {
  Serial.println("Desligando: gravando tela BRVARIO e entrando em deep sleep.");
  varioBuzzer.stop();
  flightRecorder.endFlight(varioData);
  wifiManager.end();
  tracklogBle.end();
  renderSleepLogoScreen();
  delay(250);
  enterDeepSleepAfterWeatherScreen();
}

void enterCriticalBatteryDeepSleep() {
  Serial.println("Bateria critica: tela branca segura e deep sleep.");
  varioBuzzer.stop();
  flightRecorder.endFlight(varioData);
  wifiManager.end();
  tracklogBle.end();
  renderSafeBlankSleepScreen();
  delay(250);
  enterDeepSleepAfterWeatherScreen();
}

void handleGpsConnectedSound() {
  const uint32_t now = millis();
  if (!varioData.gpsFix) {
    if (gpsFixWasActive && gpsFixLostMs == 0) {
      gpsFixLostMs = now;
    }
    gpsFixWasActive = false;
    return;
  }

  const bool longEnoughLoss = gpsFixLostMs == 0 || now - gpsFixLostMs >= kGpsReconnectSoundMinLostMs;
  if (!gpsFixWasActive && longEnoughLoss) {
    Serial.println("GPS conectado: emitindo aviso sonoro.");
    varioBuzzer.playGpsConnectedSound();
  }
  gpsFixWasActive = true;
  gpsFixLostMs = 0;
}

void handleTakeoffSound() {
  if (!flightRecorder.consumeTakeoffDetected()) {
    return;
  }
  Serial.println("Decolagem detectada: emitindo aviso sonoro.");
  if (tracklogBle.enabled() && !tracklogBle.connected() && !tracklogBle.activeTransfer()) {
    Serial.println("BLE Tracklog: desligado por inicio de voo sem app conectado.");
    tracklogBle.end();
    applyRadioStateToData();
  }
  varioBuzzer.playTakeoffSound();
  if (!screen.isFlightDisplay()) {
    Serial.println("Decolagem detectada fora da tela principal: retornando ao dashboard.");
    screen.showDashboard(varioData);
    lastUiUpdateMs = millis();
    lastFullTelemetryMs = millis();
  }
}

void resetIdlePowerWarning() {
  idlePowerWarningStartedMs = 0;
  lastIdlePowerWarningAlarmMs = 0;
  idlePowerWarningActive = false;
}

void noteUserActivity() {
  lastUserActivityMs = millis();
  resetIdlePowerWarning();
}

void handleWifiAutoOff(uint32_t now) {
  if (!wifiManager.isEnabled()) {
    return;
  }

  if (screen.keepsWifiRuntimeActive() || openWeatherClient.busy() || mapDownloadManager.busy() || flightSiteCatalogUpdater.busy()) {
    lastWifiActivityMs = now;
    return;
  }

  if (lastWifiActivityMs != 0 && now - lastWifiActivityMs >= kWifiAutoOffMs) {
    Serial.println("WiFi: desligamento automatico apos 1 minuto.");
    wifiManager.setEnabled(false);
    varioData.wifiEnabled = false;
    lastWifiActivityMs = now;
  }
}

void handleDeferredBootWifiConnect() {
  if (!wifiBootConnectPending || kSafeBootRadioRecoveryMode) {
    return;
  }
  if (screen.startupLocked()) {
    return;
  }

  wifiBootConnectPending = false;
  if (!openWeatherClient.isConfigured()) {
    Serial.println("WiFi: conexao inicial ignorada; OpenWeather sem API key.");
    return;
  }

  const String ssid = wifiManager.savedSsid();
  if (ssid.length() == 0) {
    Serial.println("WiFi: sem rede salva para conexao inicial.");
    return;
  }

  if (!wifiManager.isEnabled()) {
    wifiManager.enableRuntime(false);
  }
  if (wifiManager.isConnected() || wifiManager.state() == WifiConnectionState::Connecting) {
    return;
  }

  Serial.printf("WiFi: iniciando conexao automatica entre %u redes salvas.\n",
                static_cast<unsigned>(wifiManager.savedCredentialCount()));
  wifiManager.connectSaved();
  lastWifiActivityMs = millis();
}

void handleAutoPowerManagement(uint32_t now) {
  handleWifiAutoOff(now);

  if (flightRecorder.recording()) {
    lastMotionActivityMs = now;
    resetIdlePowerWarning();
    return;
  }

  if (tracklogBle.connected() || tracklogBle.activeTransfer()) {
    lastUserActivityMs = now;
    resetIdlePowerWarning();
    return;
  }

  if (screen.keepsStationaryRuntimeActive()) {
    lastUserActivityMs = now;
    resetIdlePowerWarning();
    return;
  }

  if (lastUserActivityMs != 0 && now - lastUserActivityMs < kAutoSleepTouchGraceMs) {
    resetIdlePowerWarning();
    return;
  }

  const bool hdopKnown = varioData.gpsHdop > 0.0F && varioData.gpsHdop < 90.0F;
  const bool gpsQualityOk = varioData.gpsFix && varioData.satellites >= kAutoSleepMotionMinSatellites &&
                            (!hdopKnown || varioData.gpsHdop <= kAutoSleepMotionMaxHdop);
  const bool gpsMotion = gpsQualityOk && varioData.groundSpeedKmh >= kAutoSleepMotionSpeedKmh;
  const bool baroMotion = varioData.sensorDataValid && fabsf(varioData.varioMs) >= kAutoSleepMotionVarioMs;
  if (gpsMotion || baroMotion) {
    lastMotionActivityMs = now;
    resetIdlePowerWarning();
    return;
  }

  const uint32_t lastActivity = lastUserActivityMs > lastMotionActivityMs ? lastUserActivityMs : lastMotionActivityMs;
  if (lastActivity == 0 || now - lastActivity < kIdlePowerWarningMs) {
    resetIdlePowerWarning();
    return;
  }

  if (!idlePowerWarningActive) {
    idlePowerWarningActive = true;
    idlePowerWarningStartedMs = now;
    lastIdlePowerWarningAlarmMs = now;
    Serial.println("Auto sleep: BRVARIO parado por 25 minutos; alarme antes do desligamento em 30 minutos.");
    varioBuzzer.playIdleWarningAlarm();
    return;
  }

  if (now - idlePowerWarningStartedMs >= kIdlePowerShutdownGraceMs) {
    Serial.println("Auto sleep: 5 minutos apos alarme sem atividade; desligando aos 30 minutos.");
    enterWeatherDeepSleep();
    return;
  }

  if (now - lastIdlePowerWarningAlarmMs >= kIdlePowerAlarmRepeatMs) {
    lastIdlePowerWarningAlarmMs = now;
    varioBuzzer.playIdleWarningAlarm();
  }
}

void handleCriticalBatterySleep(uint32_t now) {
  if (criticalBatterySleepTriggered) {
    return;
  }

  const bool inDetectedFlight = flightRecorder.recording() || varioData.trackingEnabled;
  const bool batteryReadingAvailable = batteryMonitor.valid() || varioData.batteryVoltage > 0.10F;
  const bool percentCritical = batteryMonitor.valid() && varioData.batteryPercent <= kCriticalBatterySleepPercent;
  const bool voltageCritical = batteryReadingAvailable && varioData.batteryVoltage > 0.10F &&
                               varioData.batteryVoltage <= kCriticalBatterySleepVoltage;
  const bool batteryCritical = !varioData.batteryCharging && (percentCritical || voltageCritical);

  if (!batteryCritical || inDetectedFlight) {
    criticalBatteryStartedMs = 0;
    return;
  }

  if (criticalBatteryStartedMs == 0) {
    criticalBatteryStartedMs = now;
    Serial.printf("Bateria critica detectada: %.2fV %u%%. Confirmando antes do sleep seguro.\n",
                  static_cast<double>(varioData.batteryVoltage),
                  static_cast<unsigned>(varioData.batteryPercent));
    varioBuzzer.playIdleWarningAlarm();
    return;
  }

  if (now - criticalBatteryStartedMs >= kCriticalBatteryConfirmMs) {
    criticalBatterySleepTriggered = true;
    Serial.printf("Bateria critica confirmada: %.2fV %u%%. Entrando em sleep seguro.\n",
                  static_cast<double>(varioData.batteryVoltage),
                  static_cast<unsigned>(varioData.batteryPercent));
    enterCriticalBatteryDeepSleep();
  }
}

void printTelemetry() {
  Serial.println("----- TELEMETRIA -----");
  gpsManager.printStatus(Serial);
  barometer.printStatus(Serial);
  Serial.printf("DATA: fix=%d sats=%u vario=%.3f altBaro=%.1f altGps=%.1f agl=%.1f aglGps=%.1f aglBaro=%.1f velSolo=%.1f rumo=%.0f vento=%.1fkm/h indo=%.0f q=%s press=%.2f temp=%.1f ganho=%.1f planeio=%.1f\n",
                varioData.gpsFix ? 1 : 0,
                varioData.satellites,
                varioData.varioMs,
                varioData.altitudeM,
                varioData.altitudeGpsM,
                varioData.altitudeAglM,
                varioData.altitudeGpsAglM,
                varioData.altitudeBaroAglM,
                varioData.groundSpeedKmh,
                varioData.courseDeg,
                varioData.windSpeedKmh,
                varioData.windDirectionDeg,
                windQualityName(varioData.windQuality),
                varioData.pressureHpa,
                varioData.temperatureC,
                varioData.ganhoTermicaM,
                varioData.glideRatio);
  Serial.printf("BAT: %.2fV %u%% carregando=%d valido=%d\n",
                static_cast<double>(varioData.batteryVoltage),
                static_cast<unsigned>(varioData.batteryPercent),
                varioData.batteryCharging ? 1 : 0,
                batteryMonitor.valid() ? 1 : 0);
  Serial.printf("IGC: storage=%d status=%s arquivo=%s duracao=%lus\n",
                tracklogStorageReady ? 1 : 0,
                flightRecorder.statusText(),
                flightRecorder.currentFilePath()[0] != '\0' ? flightRecorder.currentFilePath() : "---",
                static_cast<unsigned long>(varioData.elapsedSeconds));
}

void printRuntimeHealth() {
  const MainScreen::PageDebug page = screen.currentPageDebug();
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t minHeap = ESP.getMinFreeHeap();
  const uint32_t largestHeap = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t freePsram = ESP.getFreePsram();
  const uint32_t largestPsram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  const uint32_t loopStack = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));

  Serial.printf(
      "HEALTH: up=%lus cpu=%luMHz page=%u heap=%luKB min=%luKB maior=%luKB psram=%luKB maiorPs=%luKB "
      "stackLoop=%luB stackAudio=%luB stackBaro=%luB i2cTimeout=%lu\n",
      static_cast<unsigned long>(millis() / 1000UL),
      static_cast<unsigned long>(getCpuFrequencyMhz()),
      static_cast<unsigned>(page.page),
      static_cast<unsigned long>(freeHeap / 1024UL),
      static_cast<unsigned long>(minHeap / 1024UL),
      static_cast<unsigned long>(largestHeap / 1024UL),
      static_cast<unsigned long>(freePsram / 1024UL),
      static_cast<unsigned long>(largestPsram / 1024UL),
      static_cast<unsigned long>(loopStack),
      static_cast<unsigned long>(varioBuzzer.taskStackFreeBytes()),
      static_cast<unsigned long>(barometer.taskStackFreeBytes()),
      static_cast<unsigned long>(I2CBusLock::timeoutCount()));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1000);

  const esp_reset_reason_t resetReason = esp_reset_reason();
  Serial.printf("Reset anterior: %s (%d)\n", resetReasonName(resetReason), static_cast<int>(resetReason));
  setCpuFrequencyMhz(kCpuFrequencyMhz);
  Serial.println();
  Serial.println("Paraglider variometer - LilyGo EPD47 S3");
  Serial.printf("CPU ajustada para %lu MHz para reduzir consumo.\n", static_cast<unsigned long>(getCpuFrequencyMhz()));
  varioBuzzer.begin();
  preferencesReady = appPreferences.begin(kPrefsNamespace, false);
  if (preferencesReady) {
    audioUserEnabled = appPreferences.getBool(kAudioEnabledKey, true);
    Serial.printf("Audio salvo: %s.\n", audioUserEnabled ? "ligado" : "desligado");
  } else {
    Serial.println("Preferencias: NVS indisponivel, usando audio ligado como padrao.");
  }

  if (!I2CBusLock::begin()) {
    Serial.println("ERRO: mutex do barramento I2C nao foi criado.");
  }

  if (!display.begin()) {
    Serial.println("Display init failed");
    while (true) {
      delay(1000);
    }
  }
  renderBrandScreen();
  varioBuzzer.playStartupSound();

  touchReady = touchManager.begin();
  if (!touchReady) {
    Serial.println("Touch desativado: GT911 nao inicializou.");
  }

  clockManager.begin();
  gpsManager.begin();
  batteryMonitor.begin();
  barometer.begin();

  delay(180);

  pilotProfile.begin();
  thermalAssistConfig.begin();
  thermalAssistant.begin();
  weatherWindCache.begin();
  weatherLocationManager.begin();
  if (kSafeBootRadioRecoveryMode) {
    Serial.println("RECOVERY: WiFi/BLE desativados nesta compilacao para recuperar boot.");
    wifiManager.end();
  } else {
    wifiManager.begin();
    wifiBootConnectPending = true;
    Serial.println("WiFi: conexao salva sera iniciada apos a tela principal para buscar vento/meteo.");
  }
  openWeatherClient.begin(&wifiManager);
  mapDownloadManager.begin(&storageManager, &wifiManager);
  tracklogStorageReady = LittleFS.begin(false);
  if (tracklogStorageReady) {
    flightRecorder.begin(LittleFS, pilotProfile);
    FlightSiteCatalog::begin(&LittleFS);
    flightSiteCatalogUpdater.begin(&LittleFS, &wifiManager);
    Serial.println("LittleFS: pronto para arquivos IGC em /igc.");
  } else {
    FlightSiteCatalog::begin(nullptr);
    flightSiteCatalogUpdater.begin(nullptr, &wifiManager);
    Serial.println("LittleFS: montagem falhou; formatacao automatica bloqueada para proteger IGC.");
    Serial.println("LittleFS: use SISTEMA AVANCADO > FORMATAR GERAL apenas se aceitar apagar IGC internos.");
    Serial.println("LittleFS: indisponivel, tracklog IGC desativado.");
  }
  if (tracklogStorageReady && !kSafeBootRadioRecoveryMode) {
    tracklogBle.begin(&flightRecorder);
  }
  if (storageManager.begin()) {
    Serial.println("microSD: pronto para mapas offline em /maps.");
  } else {
    Serial.println("microSD: ausente ou nao montado; mapas reais ficam indisponiveis.");
  }
  flightRecorder.attachArchiveStorage(storageManager.filesystem(), "/brvario/igc");
  gpsManager.applyTo(varioData);
  weatherLocationManager.updateGpsLocation(varioData.latitudeDeg, varioData.longitudeDeg, varioData.gpsFix);
  applyWindEstimate();
  barometer.applyTo(varioData);
  updateThermalAssistant();
  updateFlightRecorder();
  updateClockSources();
  applyClock(varioData);
  updateBatteryState();
  applyRadioStateToData();
  applyAudioState();
  screen.attachAudioEditor(&varioBuzzer);
  screen.attachFirmwareUpdater(&firmwareUpdater);
  screen.attachWifiManager(&wifiManager);
  screen.attachMapDownloadManager(&mapDownloadManager);
  screen.attachOpenWeatherClient(&openWeatherClient);
  screen.attachWeatherLocationManager(&weatherLocationManager);
  screen.attachFlightSiteCatalogUpdater(&flightSiteCatalogUpdater);
  screen.attachPilotProfile(&pilotProfile);
  screen.attachFlightRecorder(&flightRecorder);
  screen.attachTracklogBleService(&tracklogBle);
  screen.attachThermalAssistConfig(&thermalAssistConfig);
  screen.attachStorageManager(&storageManager);
  screen.begin(varioData);
  applyAudioState();
  barometer.startRealtimePolling(publishRealtimeVarioAudio, nullptr);
  handleGpsConnectedSound();
  lastUiUpdateMs = millis();
  lastTelemetryMs = millis();
  lastFullTelemetryMs = millis();
  lastUserActivityMs = millis();
  lastMotionActivityMs = millis();
  lastWifiActivityMs = millis();
  lastRuntimeHealthMs = millis();
}

void loop() {
  const uint32_t now = millis();
  applyCpuMode();

  const bool touchPressed = touchManager.update();
  touchReady = touchManager.isReady();
  bool userTouchedThisLoop = false;
  if (touchPressed) {
    noteUserActivity();
    userTouchedThisLoop = true;
    const int16_t touchX = touchManager.getX();
    const int16_t touchY = touchManager.getY();
    const TouchAction feedbackAction = screen.previewTouchAction(touchX, touchY);
    if (feedbackAction == TouchAction::NextPage) {
      setCpuFrequencyIfNeeded(kMapCpuFrequencyMhz);
    }
    if (shouldPlayTouchFeedback(feedbackAction)) {
      varioBuzzer.playTouchFeedback();
    }
    Serial.printf("Touch X: %d Y: %d\n", touchX, touchY);
    if (screen.handleTouch(touchX, touchY)) {
      const TouchAction action = screen.lastTouchAction();
      touchManager.settleAfterAction();
      if (action == TouchAction::PowerOff) {
        Serial.printf("Botao touch acionado: %s\n", touchActionName(action));
        enterWeatherDeepSleep();
        return;
      }
      if (action == TouchAction::ToggleAudio) {
        audioUserEnabled = !audioUserEnabled;
        if (preferencesReady) {
          appPreferences.putBool(kAudioEnabledKey, audioUserEnabled);
        }
        applyAudioState();
      }
      applyAudioState();
      Serial.printf("Botao touch acionado: %s\n", touchActionName(action));
      lastUiUpdateMs = millis();
    } else if (!screen.pageSwipePending()) {
      const MainScreen::PageDebug debug = screen.currentPageDebug();
      Serial.printf("Touch sem botao cadastrado. Pagina=%u zonas=%u\n",
                    static_cast<unsigned>(debug.page),
                    static_cast<unsigned>(debug.zoneCount));
    }
  } else if (touchReady && touchManager.isPressed() && screen.handleTouchHold(touchManager.getX(), touchManager.getY())) {
    noteUserActivity();
    userTouchedThisLoop = true;
    const TouchAction action = screen.lastTouchAction();
    touchManager.settleAfterAction();
    if (action == TouchAction::PowerOff) {
      Serial.printf("Botao touch acionado: %s\n", touchActionName(action));
      enterWeatherDeepSleep();
      return;
    }
    if (action == TouchAction::NextPage) {
      setCpuFrequencyIfNeeded(kMapCpuFrequencyMhz);
    }
    Serial.printf("Gesto touch acionado: %s\n", touchActionName(action));
    lastUiUpdateMs = millis();
  }

  if (!userTouchedThisLoop) {
    handleAutoPowerManagement(millis());
  }

  if (screen.pageSwipePending() && touchReady && touchManager.isPressed()) {
    updateVarioAudio();
    return;
  }

  wifiManager.update();
  handleDeferredBootWifiConnect();
  gpsManager.poll();
  weatherLocationManager.updateGpsLocation(varioData.latitudeDeg, varioData.longitudeDeg, varioData.gpsFix);
  requestWeatherWhenWifiConnected(now);
  openWeatherClient.update();
  updateWeatherWindCacheFromApi();
  flightSiteCatalogUpdater.update();
  mapDownloadManager.update();
  tracklogBle.update();

  const uint32_t afterTouchNow = millis();
  const bool fullTelemetry = fullTelemetryActive();
  const bool fullTelemetryDue = fullTelemetry && (afterTouchNow - lastFullTelemetryMs >= kFullTelemetryIntervalMs);
  const bool configTelemetryDue = !fullTelemetry && (afterTouchNow - lastConfigTelemetryMs >= kConfigTelemetrySlowIntervalMs);
  if (fullTelemetryDue || configTelemetryDue) {
    updateTelemetryPipeline(fullTelemetry);
    if (fullTelemetryDue) {
      lastFullTelemetryMs = afterTouchNow;
    }
    if (!fullTelemetry) {
      lastConfigTelemetryMs = afterTouchNow;
    }
  } else {
    updateInternetClock();
    updateBatteryState();
    applyClock(varioData);
    applyRadioStateToData();
    applyAudioState();
  }
  handleCriticalBatterySleep(millis());
  updateVarioAudio();

  const uint32_t postTelemetryNow = millis();
  if (postTelemetryNow - lastUiUpdateMs >= AppConfig::kUiUpdateIntervalMs) {
    if (fullTelemetryActive()) {
      gpsManager.applyTo(varioData);
      applyWindEstimate();
      // Barometer runs after GPS so glide can use real ground speed.
      barometer.applyTo(varioData);
      updateThermalAssistant();
      updateFlightRecorder();
    }
    applyClock(varioData);
    applyRadioStateToData();
    applyAudioState();
    screen.update(varioData);
    lastUiUpdateMs = millis();
  }

  if (postTelemetryNow - lastTelemetryMs >= kTelemetryLogIntervalMs) {
    if (fullTelemetryActive()) {
      gpsManager.applyTo(varioData);
      applyWindEstimate();
      barometer.applyTo(varioData);
      updateThermalAssistant();
      updateFlightRecorder();
    }
    applyClock(varioData);
    applyRadioStateToData();
    applyAudioState();
    printTelemetry();
    lastTelemetryMs = postTelemetryNow;
  }

  if (postTelemetryNow - lastRuntimeHealthMs >= kRuntimeHealthIntervalMs) {
    printRuntimeHealth();
    lastRuntimeHealthMs = postTelemetryNow;
  }

  delay(3);
}
