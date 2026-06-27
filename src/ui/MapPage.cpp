#include "ui/MapPage.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/AppConfig.h"
#include "epd_driver.h"
#include "firasans.h"
#include "map/OfflineMapPackage.h"
#include "ui/PilotIcon.h"
#include "storage/StorageManager.h"

namespace {

static constexpr Rect_t kMapBounds = {8, 8, EPD_WIDTH - 16, EPD_HEIGHT - 16};
static constexpr Rect_t kFooterMenuBounds = {278, 472, 400, 60};
static constexpr Rect_t kTopInfoBarBounds = {136, 18, 640, 68};
static constexpr Rect_t kBatteryBounds = {820, 12, 118, 66};
static constexpr Rect_t kVarioBarBounds = {850, 86, 88, 366};
static constexpr Rect_t kVarioValueBounds = {724, 236, 132, 68};
static constexpr Rect_t kScaleBounds = {24, 468, 248, 58};
static constexpr Rect_t kDurationBounds = {328, 442, 300, 28};
static constexpr int32_t kDurationFrameHorizontalInset = 12;
static constexpr int32_t kDurationFrameVerticalInset = 2;
static constexpr int32_t kDurationFrameRadius = 6;
static constexpr Rect_t kGlideBounds = {700, 486, 188, 34};
static constexpr Rect_t kZoomInBounds = {43, 176, 64, 64};
static constexpr Rect_t kZoomOutBounds = {43, 252, 64, 64};
static constexpr Rect_t kPanUpBounds = {54, 330, 42, 42};
static constexpr Rect_t kPanDownBounds = {54, 420, 42, 42};
static constexpr Rect_t kPanLeftBounds = {10, 375, 42, 42};
static constexpr Rect_t kPanRightBounds = {98, 375, 42, 42};
static constexpr int32_t kPilotX = EPD_WIDTH / 2;
static constexpr int32_t kPilotY = 250;
static constexpr float kDegToRad = 0.01745329251994329577F;
static constexpr float kMetersPerLatDeg = 111320.0F;
static constexpr float kTrailMinDistanceM = 45.0F;
static constexpr uint32_t kTrailWindowMs = 5UL * 60UL * 1000UL;
static constexpr float kPositionDirtyDistanceM = 6.0F;
static constexpr float kHeadingDirtyDeg = 3.0F;
static constexpr float kWindDirectionDirtyDeg = 8.0F;
static constexpr float kWindSpeedDirtyKmh = 1.0F;
static constexpr float kSpeedDirtyKmh = 1.0F;
static constexpr float kVarioDirtyMs = 0.01F;
static constexpr int32_t kAltitudeDirtyM = 2;
static constexpr int32_t kMapRecenterMarginPx = 120;
static constexpr uint32_t kManualPanRecenterDelayMs = 30000UL;
static constexpr float kZoomMetersPerPixel[] = {22.0F, 14.7F, 10.9F, 8.2F, 5.8F};
static constexpr uint8_t kZoomLevelCount = sizeof(kZoomMetersPerPixel) / sizeof(kZoomMetersPerPixel[0]);
static constexpr uint8_t kDefaultZoomLevel = kZoomLevelCount - 1;
static constexpr uint8_t kPersistentZoomMinLevel = kZoomLevelCount - 3;
static constexpr char kMapPrefsNamespace[] = "mapPage";
static constexpr char kMapPrefsZoomKey[] = "zoom";

bool pointInExpandedRect(const Rect_t& rect, int32_t x, int32_t y, int32_t margin) {
  return x >= rect.x - margin && x < rect.x + rect.width + margin && y >= rect.y - margin &&
         y < rect.y + rect.height + margin;
}

float normalizeDeg(float value) {
  while (value < 0.0F) value += 360.0F;
  while (value >= 360.0F) value -= 360.0F;
  return value;
}

int32_t degToE7(float value) {
  return static_cast<int32_t>(value * 10000000.0F + (value >= 0.0F ? 0.5F : -0.5F));
}

int32_t clampInt32(int32_t value, int32_t minValue, int32_t maxValue) {
  if (value < minValue) return minValue;
  if (value > maxValue) return maxValue;
  return value;
}

uint8_t clampGray(int32_t value) {
  return static_cast<uint8_t>(clampInt32(value, 0, 15));
}

bool timerActive(uint32_t untilMs) {
  return untilMs != 0 && static_cast<int32_t>(millis() - untilMs) < 0;
}

bool isPersistentZoomLevel(uint8_t zoomLevel) {
  return zoomLevel >= kPersistentZoomMinLevel && zoomLevel < kZoomLevelCount;
}

int8_t preparedBaseCacheSlotForZoom(uint8_t zoomLevel) {
  if (!isPersistentZoomLevel(zoomLevel)) {
    return -1;
  }
  return static_cast<int8_t>(zoomLevel - kPersistentZoomMinLevel);
}

int8_t snappedVarioLevel(float value) {
  if (value >= 3.0F) return 3;
  if (value >= 2.0F) return 2;
  if (value >= 1.0F) return 1;
  if (value <= -3.0F) return -3;
  if (value <= -2.0F) return -2;
  if (value <= -1.0F) return -1;
  return 0;
}

int32_t roundedMeters(float value) {
  return static_cast<int32_t>(value + (value >= 0.0F ? 0.5F : -0.5F));
}

bool shouldShowThermalGain(float gainM, uint8_t coreConfidence) {
  return coreConfidence >= 25 && gainM > 0.5F;
}

const char* cardinalPoint(float bearingDeg) {
  static const char* kPoints[] = {"N", "NE", "L", "SE", "S", "SO", "O", "NO"};
  const uint8_t index = static_cast<uint8_t>((normalizeDeg(bearingDeg) + 22.5F) / 45.0F) & 0x07;
  return kPoints[index];
}

int32_t sinBearingX(float bearingDeg, int32_t length) {
  return static_cast<int32_t>(sinf(normalizeDeg(bearingDeg) * kDegToRad) * static_cast<float>(length));
}

int32_t cosBearingY(float bearingDeg, int32_t length) {
  return static_cast<int32_t>(-cosf(normalizeDeg(bearingDeg) * kDegToRad) * static_cast<float>(length));
}

void drawThickLine(uint8_t* fb, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color) {
  epd_draw_line(x0, y0, x1, y1, color, fb);
  epd_draw_line(x0 + 1, y0, x1 + 1, y1, color, fb);
  epd_draw_line(x0, y0 + 1, x1, y1 + 1, color, fb);
}

void drawTrailLine(uint8_t* fb, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color) {
  epd_draw_line(x0, y0, x1, y1, color, fb);
  epd_draw_line(x0 + 1, y0, x1 + 1, y1, color, fb);
  epd_draw_line(x0 - 1, y0, x1 - 1, y1, color, fb);
  epd_draw_line(x0, y0 + 1, x1, y1 + 1, color, fb);
  epd_draw_line(x0, y0 - 1, x1, y1 - 1, color, fb);
  epd_draw_line(x0 + 2, y0, x1 + 2, y1, color, fb);
  epd_draw_line(x0 - 2, y0, x1 - 2, y1, color, fb);
}

void formatDuration(char* text, size_t size, uint32_t seconds) {
  const uint32_t hours = seconds / 3600UL;
  const uint32_t minutes = (seconds / 60UL) % 60UL;
  const uint32_t secs = seconds % 60UL;
  snprintf(text,
           size,
           "%lu:%02lu:%02lu",
           static_cast<unsigned long>(hours),
           static_cast<unsigned long>(minutes),
           static_cast<unsigned long>(secs));
}

void formatClock(char* text, size_t size, uint32_t secondsOfDay) {
  const uint32_t hours = (secondsOfDay / 3600UL) % 24UL;
  const uint32_t minutes = (secondsOfDay / 60UL) % 60UL;
  snprintf(text, size, "%02lu:%02lu", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
}

}  // namespace

void MapPage::begin() {
  // Display/framebuffer is intentionally not initialized here.
  // MainScreen calls begin(display) so this page uses the global framebuffer.
  loadPreferredZoom();
}

void MapPage::begin(EpdDisplay& display) {
  display_ = &display;
  loadPreferredZoom();
}

void MapPage::attachStorageManager(StorageManager* storage) {
  storage_ = storage;
}

void MapPage::draw() {
  drawBase();
  drawDynamic();
}

void MapPage::drawBase() {
  uint8_t* fb = framebuffer();
  if (!fb) return;
  ensureMapCenter();

  epd_fill_rect(kMapBounds.x, kMapBounds.y, kMapBounds.width, kMapBounds.height, AppConfig::kWhite, fb);
  drawMapArea();
  if (!drawOfflineMapVectors()) {
    drawOfflineMapPlaceholder();
  }
  drawZoomButtons();
  drawPanButtons();
}

void MapPage::drawDynamic() {
  uint8_t* fb = framebuffer();
  if (!fb) return;
  ensureMapCenter();

  drawFlightTrail();
  drawThermalOverlay();
  drawWindDirectionOverlay();
  drawZoomButtons();
  drawPanButtons();
  drawPilotMarker();
  drawBatteryIndicator();
  drawVarioBar();
  drawTopInfoBar();
  drawScaleBar();
  drawFlightDuration();
  drawGlideRatio();
}

void MapPage::updateMockData() {
  headingDeg = normalizeDeg(headingDeg + 7.0F);
  windSpeedKmh = 16.0F + 4.0F * sinf(headingDeg * kDegToRad);
  windAvailable_ = true;
  groundSpeed = 40.0F + 3.0F * cosf(headingDeg * kDegToRad);
  vario = 1.4F + 0.7F * sinf((headingDeg + 35.0F) * kDegToRad);
  glideRatio = 7.5F + 1.2F * cosf(headingDeg * kDegToRad);
  thermalGainM = vario > 0.10F ? thermalGainM + 1.0F : 0.0F;
  elapsedSeconds += 7;
  altitude += 2;
  currentLat += 0.00002F;
  currentLon += 0.00003F;
  addTrailPoint(currentLat, currentLon);
  markDynamicDirty();
}

void MapPage::setPosition(float lat, float lon) {
  const float movedM = distanceFromCurrentM(lat, lon);
  currentLat = lat;
  currentLon = lon;
  ensureMapCenter();
  if (movedM >= kPositionDirtyDistanceM) {
    markDynamicDirty();
  }
  const uint16_t previousTrailCount = trailCount_;
  const uint16_t previousTrailHead = trailHead_;
  addTrailPoint(lat, lon);
  if (trailCount_ != previousTrailCount || trailHead_ != previousTrailHead) {
    markDynamicDirty();
  }

  int32_t x = 0;
  int32_t y = 0;
  if (!timerActive(manualPanHoldUntilMs_) && projectLatLon(lat, lon, x, y) &&
      !pointInsideMap(x, y, kMapRecenterMarginPx)) {
    mapCenterLat = lat;
    mapCenterLon = lon;
    markBaseDirty();
    markDynamicDirty();
  }
}

void MapPage::setHeading(float value) {
  const float normalized = normalizeDeg(value);
  float delta = normalized - headingDeg;
  if (delta > 180.0F) delta -= 360.0F;
  if (delta < -180.0F) delta += 360.0F;
  if (fabsf(delta) >= kHeadingDirtyDeg) {
    markDynamicDirty();
  }
  headingDeg = normalized;
}

void MapPage::setWind(float windToDirDeg, float speedKmh) {
  // O restante do firmware usa a direcao para onde o vento vai; no mapa guardamos de onde ele vem.
  const float normalizedFrom = normalizeDeg(windToDirDeg + 180.0F);
  float delta = normalizedFrom - windFromDirDeg;
  if (delta > 180.0F) delta -= 360.0F;
  if (delta < -180.0F) delta += 360.0F;
  if (!windAvailable_ || fabsf(delta) >= kWindDirectionDirtyDeg || fabsf(speedKmh - windSpeedKmh) >= kWindSpeedDirtyKmh) {
    markDynamicDirty();
  }
  windAvailable_ = true;
  windFromDirDeg = normalizedFrom;
  windSpeedKmh = speedKmh;
}

void MapPage::clearWind() {
  if (windAvailable_ || windSpeedKmh != 0.0F) {
    markDynamicDirty();
  }
  windAvailable_ = false;
  windSpeedKmh = 0.0F;
}

void MapPage::setFlightData(float altitudeMValue,
                            float altitudeAglMValue,
                            float groundSpeedKmhValue,
                            float varioMsValue,
                            float glideRatioValue,
                            float thermalGainMValue,
                            uint32_t elapsedSecondsValue) {
  const int nextAltitude = static_cast<int>(altitudeMValue + (altitudeMValue >= 0.0F ? 0.5F : -0.5F));
  const int nextAltitudeAgl = static_cast<int>(altitudeAglMValue + (altitudeAglMValue >= 0.0F ? 0.5F : -0.5F));
  const int nextGlideTenths = static_cast<int>(glideRatioValue * 10.0F + (glideRatioValue >= 0.0F ? 0.5F : -0.5F));
  const int currentGlideTenths = static_cast<int>(glideRatio * 10.0F + (glideRatio >= 0.0F ? 0.5F : -0.5F));
  const int32_t nextGainM = roundedMeters(thermalGainMValue);
  const int32_t currentGainM = roundedMeters(thermalGainM);
  const bool currentGainVisible = shouldShowThermalGain(thermalGainM, thermalCoreConfidence_);
  const bool nextGainVisible = shouldShowThermalGain(thermalGainMValue, thermalCoreConfidence_);
  if (abs(nextAltitude - altitude) >= kAltitudeDirtyM || abs(nextAltitudeAgl - altitudeAgl) >= kAltitudeDirtyM ||
      fabsf(groundSpeedKmhValue - groundSpeed) >= kSpeedDirtyKmh ||
      fabsf(varioMsValue - vario) >= kVarioDirtyMs || nextGlideTenths != currentGlideTenths ||
      nextGainM != currentGainM || nextGainVisible != currentGainVisible ||
      elapsedSecondsValue / 60UL != elapsedSeconds / 60UL) {
    markDynamicDirty();
  }
  altitude = nextAltitude;
  altitudeAgl = nextAltitudeAgl;
  groundSpeed = groundSpeedKmhValue;
  vario = varioMsValue;
  glideRatio = glideRatioValue;
  thermalGainM = thermalGainMValue;
  elapsedSeconds = elapsedSecondsValue;
}

void MapPage::setSystemStatus(uint8_t satellites,
                              uint8_t batteryPercent,
                              bool gpsFix,
                              bool trackingEnabled,
                              uint32_t timeOfDaySeconds) {
  const uint16_t currentMinute = static_cast<uint16_t>((timeOfDaySeconds_ / 60UL) % 1440UL);
  const uint16_t nextMinute = static_cast<uint16_t>((timeOfDaySeconds / 60UL) % 1440UL);
  if (satellites_ != satellites || batteryPercent_ != batteryPercent || gpsFix_ != gpsFix || trackingEnabled_ != trackingEnabled ||
      currentMinute != nextMinute) {
    markDynamicDirty();
  }
  if (trackingEnabled_ != trackingEnabled) {
    markBaseDirty();
  }
  satellites_ = satellites;
  batteryPercent_ = batteryPercent;
  timeOfDaySeconds_ = timeOfDaySeconds;
  gpsFix_ = gpsFix;
  trackingEnabled_ = trackingEnabled;
}

void MapPage::setThermalOverlay(const ThermalAssistPoint* points,
                                uint8_t count,
                                const ThermalHistoryPoint* history,
                                uint8_t historyCount,
                                float pilotEastM,
                                float pilotNorthM,
                                uint8_t coreConfidence) {
  const uint8_t nextCount = points ? (count > kMaxThermalBubbles ? kMaxThermalBubbles : count) : 0;
  const uint8_t nextHistoryCount = history ? (historyCount > kMaxThermalHistoryBubbles ? kMaxThermalHistoryBubbles : historyCount) : 0;
  bool changed = nextCount != thermalBubbleCount_ || nextHistoryCount != thermalHistoryCount_ ||
                 thermalCoreConfidence_ != coreConfidence;
  for (uint8_t i = 0; i < nextCount; ++i) {
    const float lift = points[i].liftMs;
    const float eastRel = points[i].eastM - pilotEastM;
    const float northRel = points[i].northM - pilotNorthM;
    if (i >= thermalBubbleCount_ || fabsf(thermalBubbles_[i].eastM - eastRel) > 8.0F ||
        fabsf(thermalBubbles_[i].northM - northRel) > 8.0F || fabsf(thermalBubbles_[i].liftMs - lift) > 0.15F) {
      changed = true;
    }
    thermalBubbles_[i] = {eastRel, northRel, lift, true};
  }
  for (uint8_t i = nextCount; i < kMaxThermalBubbles; ++i) {
    thermalBubbles_[i].valid = false;
  }
  thermalBubbleCount_ = nextCount;

  for (uint8_t i = 0; i < nextHistoryCount; ++i) {
    const ThermalHistoryPoint& src = history[i];
    const float eastRel = src.eastM - pilotEastM;
    const float northRel = src.northM - pilotNorthM;
    if (i >= thermalHistoryCount_ || fabsf(thermalHistory_[i].eastM - eastRel) > 10.0F ||
        fabsf(thermalHistory_[i].northM - northRel) > 10.0F ||
        fabsf(thermalHistory_[i].coreMs - src.coreMs) > 0.2F ||
        thermalHistory_[i].confidencePercent != src.confidencePercent || thermalHistory_[i].ageMinutes != src.ageMinutes ||
        thermalHistory_[i].active != src.active) {
      changed = true;
    }
    thermalHistory_[i] = {eastRel, northRel, src.coreMs, src.confidencePercent, src.ageMinutes, src.active, true};
  }
  for (uint8_t i = nextHistoryCount; i < kMaxThermalHistoryBubbles; ++i) {
    thermalHistory_[i].valid = false;
  }
  thermalHistoryCount_ = nextHistoryCount;
  thermalCoreConfidence_ = coreConfidence;
  if (changed) {
    markDynamicDirty();
  }
}

void MapPage::panByMeters(float eastM, float northM) {
  ensureMapCenter();
  const float cosLat = fmaxf(0.15F, cosf(mapCenterLat * kDegToRad));
  mapCenterLat += northM / kMetersPerLatDeg;
  mapCenterLon += eastM / (kMetersPerLatDeg * cosLat);
  manualPanHoldUntilMs_ = millis() + kManualPanRecenterDelayMs;
  markBaseDirty();
  markDynamicDirty();
}

void MapPage::panByScreenStep(int8_t eastSteps, int8_t northSteps) {
  static constexpr int32_t kPanStepPx = 112;
  const float stepMeters = static_cast<float>(kPanStepPx) * metersPerPixel();
  panByMeters(static_cast<float>(eastSteps) * stepMeters, static_cast<float>(northSteps) * stepMeters);
}

void MapPage::zoomIn() {
  if (canZoomIn()) {
    ++zoomLevel_;
    savePreferredZoom();
    markBaseDirty();
    markDynamicDirty();
  }
}

void MapPage::zoomOut() {
  if (canZoomOut()) {
    --zoomLevel_;
    savePreferredZoom();
    markBaseDirty();
    markDynamicDirty();
  }
}

bool MapPage::canZoomIn() const {
  return zoomLevel_ + 1 < kZoomLevelCount;
}

bool MapPage::canZoomOut() const {
  return zoomLevel_ > 0;
}

Rect_t MapPage::mapBounds() const {
  return kMapBounds;
}

Rect_t MapPage::dynamicBounds() const {
  return kMapBounds;
}

bool MapPage::hasPreparedBaseCache() const {
  const int8_t slot = preparedBaseCacheSlotForZoom(zoomLevel_);
  return slot >= 0 && preparedBaseMatchesCurrentView(static_cast<uint8_t>(slot));
}

bool MapPage::restorePreparedBaseCache() {
  uint8_t* fb = framebuffer();
  const int8_t slotIndex = preparedBaseCacheSlotForZoom(zoomLevel_);
  if (!fb || slotIndex < 0 || !preparedBaseMatchesCurrentView(static_cast<uint8_t>(slotIndex))) {
    return false;
  }
  const PreparedBaseCache& cache = preparedBaseCaches_[slotIndex];

  const size_t sourceStride = (static_cast<size_t>(kMapBounds.width) + 1U) / 2U;
  const size_t fbStride = static_cast<size_t>(EPD_WIDTH) / 2U;
  for (int32_t row = 0; row < kMapBounds.height; ++row) {
    const size_t srcOffset = static_cast<size_t>(row) * sourceStride;
    const size_t dstOffset = static_cast<size_t>(kMapBounds.y + row) * fbStride + static_cast<size_t>(kMapBounds.x) / 2U;
    memcpy(fb + dstOffset, cache.buffer + srcOffset, sourceStride);
  }
  return true;
}

void MapPage::savePreparedBaseCache() {
  uint8_t* fb = framebuffer();
  const int8_t slotIndex = preparedBaseCacheSlotForZoom(zoomLevel_);
  if (!fb || slotIndex < 0 || !ensurePreparedBaseCache(static_cast<uint8_t>(slotIndex))) {
    return;
  }
  PreparedBaseCache& cache = preparedBaseCaches_[slotIndex];
  ensureMapCenter();

  const size_t targetStride = (static_cast<size_t>(kMapBounds.width) + 1U) / 2U;
  const size_t fbStride = static_cast<size_t>(EPD_WIDTH) / 2U;
  for (int32_t row = 0; row < kMapBounds.height; ++row) {
    const size_t srcOffset = static_cast<size_t>(kMapBounds.y + row) * fbStride + static_cast<size_t>(kMapBounds.x) / 2U;
    const size_t dstOffset = static_cast<size_t>(row) * targetStride;
    memcpy(cache.buffer + dstOffset, fb + srcOffset, targetStride);
  }

  cache.latE7 = degToE7(mapCenterLat);
  cache.lonE7 = degToE7(mapCenterLon);
  cache.zoom = zoomLevel_;
  cache.trackingEnabled = trackingEnabled_;
  cache.valid = true;
}

void MapPage::invalidatePreparedBaseCache() {
  for (uint8_t i = 0; i < kPreparedBaseCacheSlots; ++i) {
    preparedBaseCaches_[i].valid = false;
  }
}

void MapPage::markBaseDirty() {
  baseDirty_ = true;
}

void MapPage::markDynamicDirty() {
  dynamicDirty_ = true;
}

bool MapPage::ensurePreparedBaseCache(uint8_t slot) {
  if (slot >= kPreparedBaseCacheSlots) {
    return false;
  }
  PreparedBaseCache& cache = preparedBaseCaches_[slot];
  if (cache.buffer) {
    return true;
  }

  cache.size = ((static_cast<size_t>(kMapBounds.width) + 1U) / 2U) * static_cast<size_t>(kMapBounds.height);
  cache.buffer = static_cast<uint8_t*>(heap_caps_malloc(cache.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!cache.buffer) {
    cache.buffer = static_cast<uint8_t*>(malloc(cache.size));
  }
  if (!cache.buffer) {
    cache.size = 0;
    return false;
  }

  memset(cache.buffer, 0xFF, cache.size);
  return true;
}

bool MapPage::preparedBaseMatchesCurrentView(uint8_t slot) const {
  if (slot >= kPreparedBaseCacheSlots || !mapCenterValid_) {
    return false;
  }
  const PreparedBaseCache& cache = preparedBaseCaches_[slot];
  if (!cache.valid || !cache.buffer) {
    return false;
  }
  return cache.zoom == zoomLevel_ && cache.latE7 == degToE7(mapCenterLat) && cache.lonE7 == degToE7(mapCenterLon) &&
         cache.trackingEnabled == trackingEnabled_;
}

void MapPage::loadPreferredZoom() {
  zoomLevel_ = kDefaultZoomLevel;

  Preferences prefs;
  if (!prefs.begin(kMapPrefsNamespace, true)) {
    return;
  }
  const uint8_t storedZoom = prefs.getUChar(kMapPrefsZoomKey, kDefaultZoomLevel);
  prefs.end();

  if (isPersistentZoomLevel(storedZoom)) {
    zoomLevel_ = storedZoom;
  }
}

void MapPage::savePreferredZoom() const {
  if (!isPersistentZoomLevel(zoomLevel_)) {
    return;
  }

  Preferences prefs;
  if (!prefs.begin(kMapPrefsNamespace, false)) {
    return;
  }
  prefs.putUChar(kMapPrefsZoomKey, zoomLevel_);
  prefs.end();
}

float MapPage::distanceFromCurrentM(float lat, float lon) const {
  const float cosLat = fmaxf(0.15F, cosf(currentLat * kDegToRad));
  const float northM = (lat - currentLat) * kMetersPerLatDeg;
  const float eastM = (lon - currentLon) * kMetersPerLatDeg * cosLat;
  return sqrtf(northM * northM + eastM * eastM);
}

uint8_t* MapPage::framebuffer() const {
  return display_ ? display_->framebuffer() : nullptr;
}

void MapPage::ensureMapCenter() {
  if (mapCenterValid_) {
    return;
  }
  mapCenterLat = currentLat;
  mapCenterLon = currentLon;
  mapCenterValid_ = true;
  markBaseDirty();
  markDynamicDirty();
  addTrailPoint(currentLat, currentLon);
}

void MapPage::addTrailPoint(float lat, float lon) {
  if (!mapCenterValid_) {
    return;
  }
  if (trailCount_ > 0) {
    const uint16_t lastIndex = (trailHead_ + kMaxTrailPoints - 1) % kMaxTrailPoints;
    const float lastLat = trail_[lastIndex].lat;
    const float lastLon = trail_[lastIndex].lon;
    const float cosLat = fmaxf(0.15F, cosf(mapCenterLat * kDegToRad));
    const float northM = (lat - lastLat) * kMetersPerLatDeg;
    const float eastM = (lon - lastLon) * kMetersPerLatDeg * cosLat;
    if (sqrtf(northM * northM + eastM * eastM) < kTrailMinDistanceM) {
      return;
    }
  }

  trail_[trailHead_] = {lat, lon, millis(), true};
  trailHead_ = (trailHead_ + 1) % kMaxTrailPoints;
  if (trailCount_ < kMaxTrailPoints) {
    ++trailCount_;
  }
}

bool MapPage::projectLatLon(float lat, float lon, int32_t& x, int32_t& y) const {
  if (!mapCenterValid_) {
    return false;
  }
  const float cosLat = fmaxf(0.15F, cosf(mapCenterLat * kDegToRad));
  const float eastM = (lon - mapCenterLon) * kMetersPerLatDeg * cosLat;
  const float northM = (lat - mapCenterLat) * kMetersPerLatDeg;
  const float mpp = metersPerPixel();
  x = kPilotX + static_cast<int32_t>(eastM / mpp + (eastM >= 0.0F ? 0.5F : -0.5F));
  y = kPilotY - static_cast<int32_t>(northM / mpp + (northM >= 0.0F ? 0.5F : -0.5F));
  return true;
}

float MapPage::metersPerPixel() const {
  return kZoomMetersPerPixel[zoomLevel_ < kZoomLevelCount ? zoomLevel_ : 2];
}

bool MapPage::pointInsideMap(int32_t x, int32_t y, int32_t margin) const {
  const Rect_t movingVarioValueBounds = {kVarioValueBounds.x,
                                         kVarioBarBounds.y,
                                         kVarioValueBounds.width + 16,
                                         kVarioBarBounds.height};
  if (pointInExpandedRect(kFooterMenuBounds, x, y, margin) || pointInExpandedRect(kTopInfoBarBounds, x, y, margin) ||
      pointInExpandedRect(kBatteryBounds, x, y, margin) || pointInExpandedRect(kVarioBarBounds, x, y, margin) ||
      pointInExpandedRect(movingVarioValueBounds, x, y, margin) ||
      pointInExpandedRect(kScaleBounds, x, y, margin) || pointInExpandedRect(kDurationBounds, x, y, margin) ||
      pointInExpandedRect(kGlideBounds, x, y, margin) || pointInExpandedRect(kZoomInBounds, x, y, margin) ||
      pointInExpandedRect(kZoomOutBounds, x, y, margin) ||
      (showPanControls() && (pointInExpandedRect(kPanUpBounds, x, y, margin) || pointInExpandedRect(kPanDownBounds, x, y, margin) ||
                             pointInExpandedRect(kPanLeftBounds, x, y, margin) || pointInExpandedRect(kPanRightBounds, x, y, margin)))) {
    return false;
  }
  return x >= kMapBounds.x + margin && x < kMapBounds.x + kMapBounds.width - margin && y >= kMapBounds.y + margin &&
         y < kMapBounds.y + kMapBounds.height - margin;
}

void MapPage::drawText(const char* text, int32_t x, int32_t baselineY) {
  uint8_t* fb = framebuffer();
  if (!fb || !display_ || !text) return;

  const int32_t boxW = clampInt32(static_cast<int32_t>(strlen(text)) * 12 + 18, 58, 112);
  const int32_t boxH = 28;
  const Rect_t box = {x, baselineY - 22, boxW, boxH};
  epd_fill_rect(box.x, box.y, box.width, box.height, AppConfig::kWhite, fb);
  display_->drawRoundRect(box, 6, AppConfig::kBlack);
  display_->drawSmallTextBold(text, box.x + 9, box.y + 6, 2, AppConfig::kBlack);
}

void MapPage::drawCenteredText(const char* text, int32_t centerX, int32_t baselineY) {
  uint8_t* fb = framebuffer();
  if (!fb || !text) return;

  FontProperties props;
  props.fg_color = 0;
  props.bg_color = 15;
  props.fallback_glyph = '?';
  props.flags = 0;

  int32_t measureX = 0;
  int32_t measureY = baselineY;
  int32_t x1 = 0;
  int32_t y1 = 0;
  int32_t w = 0;
  int32_t h = 0;
  get_text_bounds(&FiraSans, text, &measureX, &measureY, &x1, &y1, &w, &h, &props);
  drawText(text, centerX - (x1 + w / 2), baselineY);
}

void MapPage::drawSmallValue(const char* label, const char* value, const char* unit, int32_t x, int32_t y, int32_t w) {
  if (!display_) return;
  uint8_t* fb = framebuffer();
  const int32_t labelW = clampInt32(static_cast<int32_t>(strlen(label)) * 12 + 24, 58, w - 18);
  const int32_t labelX = x + (w - labelW) / 2;
  const int32_t labelY = y - 18;
  if (fb) {
    epd_fill_rect(labelX, labelY, labelW, 24, AppConfig::kWhite, fb);
    epd_draw_rect(labelX, labelY, labelW, 24, AppConfig::kBlack, fb);
  }
  display_->drawSmallTextBoldAligned(label, x + w / 2, labelY + 4, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  const int32_t valueWidth = value && value[0] != '\0' ? static_cast<int32_t>(strlen(value)) * 36 - 6 : 0;
  const int32_t unitWidth = unit && unit[0] != '\0' ? static_cast<int32_t>(strlen(unit)) * 6 - 1 : 0;
  const int32_t gap = unitWidth > 0 ? 6 : 0;
  const int32_t groupWidth = valueWidth + gap + unitWidth;
  const int32_t groupX = x + (w - groupWidth) / 2;
  const int32_t valueCenter = groupX + valueWidth / 2;
  display_->drawSmallTextBoldAligned(value, valueCenter, y + 15, 6, AppConfig::kBlack, EpdDisplay::Align::Center);
  if (unit && unit[0] != '\0') {
    display_->drawSmallTextBold(unit, groupX + valueWidth + gap, y + 52, 1, AppConfig::kBlack);
  }
}

void MapPage::drawArrow(int32_t cx, int32_t cy, float bearingDeg, int32_t length, uint8_t color, bool filledHead) {
  uint8_t* fb = framebuffer();
  if (!fb) return;

  const int32_t tipX = cx + sinBearingX(bearingDeg, length);
  const int32_t tipY = cy + cosBearingY(bearingDeg, length);
  const int32_t tailX = cx - sinBearingX(bearingDeg, length / 4);
  const int32_t tailY = cy - cosBearingY(bearingDeg, length / 4);
  drawThickLine(fb, tailX, tailY, tipX, tipY, color);

  const float left = normalizeDeg(bearingDeg + 150.0F);
  const float right = normalizeDeg(bearingDeg - 150.0F);
  const int32_t leftX = tipX + sinBearingX(left, 13);
  const int32_t leftY = tipY + cosBearingY(left, 13);
  const int32_t rightX = tipX + sinBearingX(right, 13);
  const int32_t rightY = tipY + cosBearingY(right, 13);
  if (filledHead) {
    epd_fill_triangle(tipX, tipY, leftX, leftY, rightX, rightY, color, fb);
  } else {
    epd_draw_line(tipX, tipY, leftX, leftY, color, fb);
    epd_draw_line(tipX, tipY, rightX, rightY, color, fb);
  }
}

void MapPage::drawMapArea() {
  uint8_t* fb = framebuffer();
  if (!fb) return;

  epd_draw_rect(kMapBounds.x, kMapBounds.y, kMapBounds.width, kMapBounds.height, AppConfig::kBlack, fb);
  epd_draw_rect(kMapBounds.x + 1, kMapBounds.y + 1, kMapBounds.width - 2, kMapBounds.height - 2, AppConfig::kBlack, fb);
}

bool MapPage::drawOfflineMapVectors() {
  if (!display_ || !storage_) {
    return false;
  }
  fs::FS* fs = storage_->filesystem();
  if (!fs) {
    return false;
  }

  OfflineMapPackage::View view;
  view.centerLat = mapCenterLat;
  view.centerLon = mapCenterLon;
  view.metersPerPixel = metersPerPixel();
  view.pilotX = kPilotX;
  view.pilotY = kPilotY;
  view.bounds = kMapBounds;

  OfflineMapRenderStats stats;
  if (!OfflineMapPackage::renderCovering(*fs, *display_, view, stats) || !stats.foundPackage) {
    return false;
  }

  return true;
}

void MapPage::drawFlightTrail() {
  uint8_t* fb = framebuffer();
  if (!fb || trailCount_ < 2) {
    return;
  }

  const uint32_t now = millis();
  int32_t prevX = 0;
  int32_t prevY = 0;
  bool prevOk = false;
  const uint16_t start = (trailHead_ + kMaxTrailPoints - trailCount_) % kMaxTrailPoints;
  for (uint16_t i = 0; i < trailCount_; ++i) {
    const uint16_t index = (start + i) % kMaxTrailPoints;
    if (!trail_[index].valid || now - trail_[index].timestampMs > kTrailWindowMs) {
      prevOk = false;
      continue;
    }
    int32_t x = 0;
    int32_t y = 0;
    const bool ok = projectLatLon(trail_[index].lat, trail_[index].lon, x, y) && pointInsideMap(x, y, 6);
    if (ok && prevOk) {
      drawTrailLine(fb, prevX, prevY, x, y, AppConfig::kBlack);
    }
    if (ok) {
      epd_fill_circle(x, y, 2, AppConfig::kBlack, fb);
    }
    prevX = x;
    prevY = y;
    prevOk = ok;
  }
}

void MapPage::drawWindDirectionOverlay() {
  uint8_t* fb = framebuffer();
  if (!fb || !windAvailable_ || windSpeedKmh <= 0.5F) {
    return;
  }

  const int32_t referenceX = kPilotX;
  const int32_t referenceY = kPilotY;

  const float fromRad = normalizeDeg(windFromDirDeg) * kDegToRad;
  const float dx = sinf(fromRad);
  const float dy = -cosf(fromRad);
  const int32_t left = kMapBounds.x + 10;
  const int32_t right = kMapBounds.x + kMapBounds.width - 11;
  const int32_t top = kMapBounds.y + 10;
  const int32_t bottom = kMapBounds.y + kMapBounds.height - 11;

  float distanceToBorder = 100000.0F;
  if (dx > 0.001F) {
    distanceToBorder = fminf(distanceToBorder, (static_cast<float>(right) - static_cast<float>(referenceX)) / dx);
  } else if (dx < -0.001F) {
    distanceToBorder = fminf(distanceToBorder, (static_cast<float>(left) - static_cast<float>(referenceX)) / dx);
  }
  if (dy > 0.001F) {
    distanceToBorder = fminf(distanceToBorder, (static_cast<float>(bottom) - static_cast<float>(referenceY)) / dy);
  } else if (dy < -0.001F) {
    distanceToBorder = fminf(distanceToBorder, (static_cast<float>(top) - static_cast<float>(referenceY)) / dy);
  }
  if (distanceToBorder < 70.0F || distanceToBorder > 2000.0F) {
    return;
  }

  float pilotGap = distanceToBorder * 0.25F;
  if (pilotGap < 52.0F) {
    pilotGap = 52.0F;
  }
  if (pilotGap > distanceToBorder - 28.0F) {
    pilotGap = distanceToBorder - 28.0F;
  }

  const int32_t startX = static_cast<int32_t>(static_cast<float>(referenceX) + dx * distanceToBorder + 0.5F);
  const int32_t startY = static_cast<int32_t>(static_cast<float>(referenceY) + dy * distanceToBorder + 0.5F);
  const int32_t tipX = static_cast<int32_t>(static_cast<float>(referenceX) + dx * pilotGap + 0.5F);
  const int32_t tipY = static_cast<int32_t>(static_cast<float>(referenceY) + dy * pilotGap + 0.5F);

  drawThickLine(fb, startX, startY, tipX, tipY, AppConfig::kBlack);

  const float tipBearing = normalizeDeg(windFromDirDeg + 180.0F);
  const int32_t headLen = 20;
  const int32_t leftX = tipX + sinBearingX(tipBearing + 150.0F, headLen);
  const int32_t leftY = tipY + cosBearingY(tipBearing + 150.0F, headLen);
  const int32_t rightX = tipX + sinBearingX(tipBearing - 150.0F, headLen);
  const int32_t rightY = tipY + cosBearingY(tipBearing - 150.0F, headLen);
  epd_fill_triangle(tipX, tipY, leftX, leftY, rightX, rightY, AppConfig::kBlack, fb);
}

void MapPage::drawOfflineMapPlaceholder() {
  uint8_t* fb = framebuffer();
  if (!fb) return;

  // FUTURO:
  // - converter currentLat/currentLon para tile x/y/z;
  // - carregar tiles do microSD em /maps/opentopo/zXX/xYYYY/yZZZZ.bin;
  // - manter um cache pequeno de tiles ao redor da posicao;
  // - opcionalmente baixar tiles via Wi-Fi numa pagina "Download Mapas".

  const int32_t contour1[][2] = {{72, 210}, {140, 180}, {230, 192}, {320, 162}, {430, 178}, {530, 150}, {640, 176}, {760, 154}, {890, 190}};
  const int32_t contour2[][2] = {{58, 325}, {150, 292}, {250, 314}, {350, 280}, {470, 300}, {580, 268}, {710, 292}, {876, 260}};
  const int32_t contour3[][2] = {{100, 390}, {210, 360}, {320, 372}, {442, 342}, {560, 356}, {680, 330}, {840, 344}};
  const int32_t road[][2] = {{40, 120}, {132, 150}, {216, 142}, {334, 210}, {450, 232}, {560, 292}, {740, 304}, {914, 362}};
  const int32_t river[][2] = {{52, 430}, {170, 412}, {270, 425}, {382, 390}, {510, 404}, {650, 382}, {850, 404}};

  auto drawPolyline = [fb](const int32_t points[][2], uint8_t count, uint8_t color, bool thick) {
    for (uint8_t i = 1; i < count; ++i) {
      if (thick) {
        drawThickLine(fb, points[i - 1][0], points[i - 1][1], points[i][0], points[i][1], color);
      } else {
        epd_draw_line(points[i - 1][0], points[i - 1][1], points[i][0], points[i][1], color, fb);
      }
    }
  };

  drawPolyline(contour1, sizeof(contour1) / sizeof(contour1[0]), AppConfig::kMid, false);
  drawPolyline(contour2, sizeof(contour2) / sizeof(contour2[0]), AppConfig::kMid, false);
  drawPolyline(contour3, sizeof(contour3) / sizeof(contour3[0]), AppConfig::kMid, false);
  drawPolyline(road, sizeof(road) / sizeof(road[0]), AppConfig::kBlack, true);
  drawPolyline(river, sizeof(river) / sizeof(river[0]), AppConfig::kLight, true);

  epd_draw_circle(214, 142, 9, AppConfig::kBlack, fb);
  epd_fill_circle(214, 142, 4, AppConfig::kBlack, fb);
  drawText("RAMPA", 230, 148);
  drawText("CIDADE", 704, 284);
  drawText("RIO", 690, 396);
  drawText("TRILHA", 390, 224);
}

void MapPage::drawPilotMarker() {
  uint8_t* fb = framebuffer();
  if (!fb) return;

  int32_t pilotX = kPilotX;
  int32_t pilotY = kPilotY;
  projectLatLon(currentLat, currentLon, pilotX, pilotY);
  if (!pointInsideMap(pilotX, pilotY, 18)) {
    pilotX = kPilotX;
    pilotY = kPilotY;
  }

  epd_fill_circle(pilotX, pilotY, 7, AppConfig::kWhite, fb);
  PilotIcon::draw(fb, pilotX, pilotY, headingDeg, 29, 18, 17, AppConfig::kBlack);
}

void MapPage::drawTopInfoBar() {
  uint8_t* fb = framebuffer();
  if (!fb || !display_) return;

  const int32_t y = kTopInfoBarBounds.y + 8;
  const int32_t x0 = kTopInfoBarBounds.x;
  const int32_t widths[] = {160, 160, 160, 160};
  int32_t x = x0;
  char value[24];

  epd_fill_rect(kTopInfoBarBounds.x, kTopInfoBarBounds.y, kTopInfoBarBounds.width, kTopInfoBarBounds.height, AppConfig::kWhite, fb);
  display_->drawRoundRect(kTopInfoBarBounds, 9, AppConfig::kBlack);
  int32_t separatorX = x0;
  for (uint8_t i = 0; i < 3; ++i) {
    separatorX += widths[i];
    epd_draw_line(separatorX, kTopInfoBarBounds.y + 5, separatorX, kTopInfoBarBounds.y + kTopInfoBarBounds.height - 6, AppConfig::kBlack, fb);
  }

  snprintf(value, sizeof(value), "%d", altitude);
  drawSmallValue("ALT", value, "m", x, y, widths[0]);
  x += widths[0];

  snprintf(value, sizeof(value), "%d", altitudeAgl);
  drawSmallValue("AGL", value, "m", x, y, widths[1]);
  x += widths[1];

  snprintf(value, sizeof(value), "%.0f", static_cast<double>(groundSpeed));
  drawSmallValue("VEL", value, "km/h", x, y, widths[2]);
  x += widths[2];

  if (windAvailable_ && windSpeedKmh > 0.5F) {
    snprintf(value, sizeof(value), "%.0f", static_cast<double>(windSpeedKmh));
    const char* directionText = cardinalPoint(windFromDirDeg);
    const int32_t labelW = clampInt32(static_cast<int32_t>(strlen("VENTO")) * 12 + 24, 58, widths[3] - 18);
    const int32_t labelX = x + (widths[3] - labelW) / 2;
    const int32_t labelY = y - 18;
    epd_fill_rect(labelX, labelY, labelW, 24, AppConfig::kWhite, fb);
    display_->drawRoundRect({labelX, labelY, labelW, 24}, 5, AppConfig::kBlack);
    display_->drawSmallTextBoldAligned("VENTO", x + widths[3] / 2, labelY + 4, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
    display_->drawSmallTextBoldAligned(value, x + 50, y + 15, 6, AppConfig::kBlack, EpdDisplay::Align::Center);
    display_->drawSmallTextBold("km/h", x + 92, y + 52, 1, AppConfig::kBlack);
    display_->drawSmallTextBoldAligned(directionText, x + 126, y + 26, 4, AppConfig::kBlack, EpdDisplay::Align::Center);
  } else {
    drawSmallValue("VENTO", "", "", x, y, widths[3]);
  }
}

void MapPage::drawBatteryIndicator() {
  uint8_t* fb = framebuffer();
  if (!fb || !display_) return;

  char value[24];
  char clockText[8];
  const Rect_t box = kBatteryBounds;
  const int32_t batX = box.x + 8;
  const int32_t batY = box.y + 8;
  formatClock(clockText, sizeof(clockText), timeOfDaySeconds_);
  epd_fill_rect(box.x, box.y, box.width, box.height, AppConfig::kWhite, fb);
  display_->drawRoundRect(box, 9, AppConfig::kBlack);
  epd_draw_rect(batX, batY, 38, 22, AppConfig::kBlack, fb);
  epd_fill_rect(batX + 38, batY + 7, 5, 8, AppConfig::kBlack, fb);
  const int32_t fillW = clampInt32(static_cast<int32_t>(batteryPercent_) * 30 / 100, 1, 30);
  epd_fill_rect(batX + 4, batY + 4, fillW, 14, AppConfig::kBlack, fb);
  snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(batteryPercent_));
  display_->drawSmallTextBold(value, box.x + 58, box.y + 11, 2, AppConfig::kBlack);
  epd_draw_line(box.x + 8, box.y + 36, box.x + box.width - 8, box.y + 36, AppConfig::kLight, fb);
  display_->drawSmallTextBoldAligned(clockText,
                                     box.x + box.width / 2,
                                     box.y + 41,
                                     3,
                                     AppConfig::kBlack,
                                     EpdDisplay::Align::Center);
}

void MapPage::drawVarioBar() {
  uint8_t* fb = framebuffer();
  if (!fb || !display_) return;

  const Rect_t box = kVarioBarBounds;
  const int32_t barX = box.x + 16;
  const int32_t barY = box.y + 18;
  const int32_t barW = 40;
  const int32_t barH = box.height - 36;
  static constexpr int32_t kVarioMaxDisplayMs = 4;
  static constexpr int32_t kVarioLabelScale = 2;
  static constexpr int32_t kVarioLabelStep = 20;
  static constexpr int32_t kVarioLabelPaddingY = 10;
  static constexpr int32_t kVarioLabelW = 24;
  static constexpr int32_t kVarioLabelGlyphH = 7 * kVarioLabelScale;
  static constexpr int32_t kVarioLabelTextH = kVarioLabelGlyphH + 4 * kVarioLabelStep;
  const Rect_t labelBox = {barX + barW,
                           barY + (barH - (kVarioLabelTextH + 2 * kVarioLabelPaddingY)) / 2,
                           kVarioLabelW,
                           kVarioLabelTextH + 2 * kVarioLabelPaddingY};
  const int32_t zeroY = barY + barH / 2;
  const int32_t tickStepPx = (barH / 2 - 5) / kVarioMaxDisplayMs;
  const int8_t snapLevel = snappedVarioLevel(vario);
  const int32_t indicatorY = zeroY - static_cast<int32_t>(snapLevel) * tickStepPx;
  const Rect_t valueBox = {kVarioValueBounds.x,
                           indicatorY - kVarioValueBounds.height / 2,
                           kVarioValueBounds.width,
                           kVarioValueBounds.height};

  char value[24];
  snprintf(value, sizeof(value), "%+.1f", static_cast<double>(vario));

  epd_fill_rect(barX, barY, barW, barH, AppConfig::kWhite, fb);
  epd_draw_rect(barX, barY, barW, barH, AppConfig::kBlack, fb);

  const float limitedFill = fmaxf(-static_cast<float>(kVarioMaxDisplayMs),
                                  fminf(static_cast<float>(kVarioMaxDisplayMs), vario));
  int32_t fillPx =
      static_cast<int32_t>(fabsf(limitedFill) * static_cast<float>(barH / 2 - 5) /
                               static_cast<float>(kVarioMaxDisplayMs) +
                           0.5F);
  if (fabsf(limitedFill) >= 0.01F && fillPx < 1) {
    fillPx = 1;
  }
  const int32_t fillTopY = zeroY - fillPx;
  const int32_t fillBottomY = zeroY + fillPx;
  if (fillPx > 0) {
    if (limitedFill > 0.0F) {
      epd_fill_rect(barX + 4, fillTopY, barW - 8, fillPx, AppConfig::kBlack, fb);
    } else if (limitedFill < 0.0F) {
      epd_fill_rect(barX + 4, zeroY, barW - 8, fillPx, AppConfig::kBlack, fb);
    }
  }

  epd_draw_line(barX - 6, zeroY, barX + barW + 6, zeroY, AppConfig::kBlack, fb);
  epd_draw_line(barX - 6, zeroY + 1, barX + barW + 6, zeroY + 1, AppConfig::kBlack, fb);
  for (int8_t tick = -kVarioMaxDisplayMs; tick <= kVarioMaxDisplayMs; ++tick) {
    if (tick == 0) continue;
    const int32_t y = zeroY - tick * tickStepPx;
    if (y > barY + 4 && y < barY + barH - 4) {
      const bool covered = (limitedFill > 0.0F && tick > 0 && y >= fillTopY && y <= zeroY) ||
                           (limitedFill < 0.0F && tick < 0 && y >= zeroY && y <= fillBottomY);
      const uint8_t tickColor = covered ? AppConfig::kWhite : AppConfig::kBlack;
      epd_draw_line(barX + 3, y, barX + 13, y, tickColor, fb);
      epd_draw_line(barX + 3, y + 1, barX + 13, y + 1, tickColor, fb);
    }
  }

  const char label[] = "VARIO";
  epd_fill_rect(labelBox.x, labelBox.y, labelBox.width, labelBox.height, AppConfig::kWhite, fb);
  epd_draw_rect(labelBox.x, labelBox.y, labelBox.width, labelBox.height, AppConfig::kBlack, fb);
  for (uint8_t i = 0; i < 5; ++i) {
    char letter[2] = {label[i], '\0'};
    display_->drawSmallTextBoldAligned(letter,
                                       labelBox.x + labelBox.width / 2,
                                       labelBox.y + kVarioLabelPaddingY + static_cast<int32_t>(i) * kVarioLabelStep,
                                       kVarioLabelScale,
                                       AppConfig::kBlack, EpdDisplay::Align::Center);
  }

  const int32_t valueRight = valueBox.x + valueBox.width - 1;
  const int32_t valueCenterY = valueBox.y + valueBox.height / 2;
  const int32_t pointerTipX = barX - 2;
  const int32_t pointerHalfH = 18;
  epd_fill_rect(valueBox.x, valueBox.y, valueBox.width, valueBox.height, AppConfig::kWhite, fb);
  epd_fill_triangle(valueRight,
                    valueCenterY - pointerHalfH,
                    valueRight,
                    valueCenterY + pointerHalfH,
                    pointerTipX,
                    indicatorY,
                    AppConfig::kWhite,
                    fb);
  display_->drawRoundRect(valueBox, 8, AppConfig::kLight);
  epd_draw_line(valueRight, valueBox.y, valueRight, valueCenterY - pointerHalfH, AppConfig::kLight, fb);
  epd_draw_line(valueRight, valueCenterY + pointerHalfH, valueRight, valueBox.y + valueBox.height - 1, AppConfig::kLight, fb);
  epd_draw_line(valueRight, valueCenterY - pointerHalfH, pointerTipX, indicatorY, AppConfig::kLight, fb);
  epd_draw_line(valueRight, valueCenterY + pointerHalfH, pointerTipX, indicatorY, AppConfig::kLight, fb);
  display_->drawSmallTextBoldAligned(value,
                                     valueBox.x + valueBox.width / 2,
                                     valueBox.y + 16,
                                     5,
                                     AppConfig::kBlack,
                                     EpdDisplay::Align::Center);
}

void MapPage::drawThermalOverlay() {
  uint8_t* fb = framebuffer();
  if (!fb) return;

  const float mpp = metersPerPixel();

  for (uint8_t i = 0; i < thermalHistoryCount_; ++i) {
    const ThermalHistoryBubble& history = thermalHistory_[i];
    if (!history.valid || history.confidencePercent == 0) {
      continue;
    }
    const int32_t x = kPilotX + static_cast<int32_t>(history.eastM / mpp + 0.5F);
    const int32_t y = kPilotY - static_cast<int32_t>(history.northM / mpp + 0.5F);
    if (!pointInsideMap(x, y, 18)) {
      continue;
    }

    const int32_t r = clampInt32(static_cast<int32_t>(history.confidencePercent / 10U) + 5, 6, 15);
    const uint8_t color = history.active ? AppConfig::kBlack : (history.confidencePercent >= 55 ? AppConfig::kMid : AppConfig::kLight);
    epd_draw_circle(x, y, r, color, fb);
    if (history.active) {
      epd_draw_circle(x, y, r + 3, AppConfig::kBlack, fb);
    }
    if (history.coreMs >= 1.0F || history.confidencePercent >= 70) {
      epd_fill_circle(x, y, clampInt32(r / 2, 3, 7), color, fb);
    }
    if (display_ && (history.active || i < 3)) {
      char label[8];
      if (history.ageMinutes == 0) {
        snprintf(label, sizeof(label), "AG");
      } else {
        snprintf(label, sizeof(label), "%um", static_cast<unsigned>(history.ageMinutes));
      }
      display_->drawSmallTextBoldAligned(label, x, y + r + 4, 1, AppConfig::kBlack, EpdDisplay::Align::Center);
    }
  }

  for (uint8_t i = 0; i < thermalBubbleCount_; ++i) {
    if (!thermalBubbles_[i].valid || thermalBubbles_[i].liftMs <= 0.15F) {
      continue;
    }
    const float limitedLift = fminf(thermalBubbles_[i].liftMs, 5.0F);
    const int32_t r = limitedLift <= 2.5F
                          ? clampInt32(static_cast<int32_t>(5.0F + limitedLift * 3.6F + 0.5F), 5, 14)
                          : clampInt32(static_cast<int32_t>(14.0F + (limitedLift - 2.5F) * 3.2F + 0.5F), 14, 22);
    const int32_t x = kPilotX + static_cast<int32_t>(thermalBubbles_[i].eastM / mpp + 0.5F);
    const int32_t y = kPilotY - static_cast<int32_t>(thermalBubbles_[i].northM / mpp + 0.5F);
    if (!pointInsideMap(x, y, r + 2)) {
      continue;
    }
    const uint8_t color = thermalBubbles_[i].liftMs >= 2.0F ? AppConfig::kBlack : 0x55;
    epd_draw_circle(x, y, r, color, fb);
    if (thermalBubbles_[i].liftMs >= 1.0F) {
      epd_fill_circle(x, y, r / 2, color, fb);
    }
  }

  if (display_ && thermalCoreConfidence_ > 0) {
    char text[18];
    snprintf(text, sizeof(text), "NUCLEO %u%%", static_cast<unsigned>(thermalCoreConfidence_));
    const Rect_t confidenceBox = {kPilotX - 72, 86, 144, 30};
    epd_fill_rect(confidenceBox.x, confidenceBox.y, confidenceBox.width, confidenceBox.height, AppConfig::kWhite, fb);
    display_->drawRoundRect(confidenceBox, 7, AppConfig::kBlack);
    display_->drawSmallTextBoldAligned(text,
                                       confidenceBox.x + confidenceBox.width / 2,
                                       confidenceBox.y + 7,
                                       2,
                                       AppConfig::kBlack,
                                       EpdDisplay::Align::Center);
  }
}

void MapPage::drawScaleBar() {
  uint8_t* fb = framebuffer();
  if (!fb || !display_) return;

  const int32_t scalePx = 220;
  const int32_t halfPx = scalePx / 2;
  const float mapWidthMeters = static_cast<float>(kMapBounds.width) * metersPerPixel();

  const int32_t x = kScaleBounds.x + 10;
  const int32_t y = kScaleBounds.y + 42;
  for (uint8_t i = 0; i < 4; ++i) {
    epd_draw_line(x, y + i, x + scalePx, y + i, AppConfig::kBlack, fb);
  }
  for (uint8_t i = 0; i < 3; ++i) {
    epd_draw_line(x + i, y - 9, x + i, y + 10, AppConfig::kBlack, fb);
    epd_draw_line(x + halfPx + i, y - 6, x + halfPx + i, y + 7, AppConfig::kBlack, fb);
    epd_draw_line(x + scalePx + i, y - 9, x + scalePx + i, y + 10, AppConfig::kBlack, fb);
  }

  char label[24];
  if (mapWidthMeters < 1000.0F) {
    snprintf(label, sizeof(label), "ESCALA %.0fm", static_cast<double>(mapWidthMeters));
  } else if (mapWidthMeters < 10000.0F) {
    snprintf(label, sizeof(label), "ESCALA %.1fkm", static_cast<double>(mapWidthMeters / 1000.0F));
  } else {
    snprintf(label, sizeof(label), "ESCALA %.0fkm", static_cast<double>(mapWidthMeters / 1000.0F));
  }
  const int32_t labelW = clampInt32(static_cast<int32_t>(strlen(label)) * 12 + 24, 120, 210);
  const int32_t labelX = x + scalePx / 2 - labelW / 2;
  const int32_t labelY = y - 40;
  epd_fill_rect(labelX, labelY, labelW, 28, AppConfig::kWhite, fb);
  display_->drawRoundRect({labelX, labelY, labelW, 28}, 6, AppConfig::kLight);
  display_->drawSmallTextBoldAligned(label, x + scalePx / 2, labelY + 6, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
}

void MapPage::drawFlightDuration() {
  uint8_t* fb = framebuffer();
  if (!display_ || !fb) return;

  char text[34];
  char duration[16];
  formatDuration(duration, sizeof(duration), trackingEnabled_ ? elapsedSeconds : 0UL);
  snprintf(text, sizeof(text), "DURAÇAO VOO %s", duration);
  epd_fill_rect(kDurationBounds.x, kDurationBounds.y, kDurationBounds.width, kDurationBounds.height, AppConfig::kWhite, fb);
  const Rect_t frame = {kDurationBounds.x + kDurationFrameHorizontalInset,
                        kDurationBounds.y + kDurationFrameVerticalInset,
                        kDurationBounds.width - kDurationFrameHorizontalInset * 2,
                        kDurationBounds.height - kDurationFrameVerticalInset * 2};
  display_->drawRoundRect(frame, kDurationFrameRadius, AppConfig::kLight);
  display_->drawSmallTextBoldAligned(text,
                                     kDurationBounds.x + kDurationBounds.width / 2,
                                     kDurationBounds.y + 6,
                                     2,
                                     AppConfig::kBlack,
                                     EpdDisplay::Align::Center);
}

void MapPage::drawGlideRatio() {
  uint8_t* fb = framebuffer();
  if (!display_ || !fb) return;

  const bool showGain = shouldShowThermalGain(thermalGainM, thermalCoreConfidence_);
  char value[16];
  if (showGain) {
    snprintf(value, sizeof(value), "+%ldM", static_cast<long>(roundedMeters(thermalGainM)));
  } else {
    snprintf(value, sizeof(value), "%.1f", static_cast<double>(glideRatio));
  }

  epd_fill_rect(kGlideBounds.x, kGlideBounds.y, kGlideBounds.width, kGlideBounds.height, AppConfig::kWhite, fb);
  display_->drawRoundRect(kGlideBounds, 7, AppConfig::kLight);
  display_->drawSmallTextBold(showGain ? "GANHO" : "PLANEIO", kGlideBounds.x + 10, kGlideBounds.y + 9, 2, AppConfig::kBlack);
  display_->drawSmallTextBoldAligned(value,
                                     kGlideBounds.x + kGlideBounds.width - 12,
                                     kGlideBounds.y + (showGain ? 7 : 5),
                                     showGain ? 2 : 3,
                                     AppConfig::kBlack,
                                     EpdDisplay::Align::Right);
}

void MapPage::drawZoomButtons() {
  uint8_t* fb = framebuffer();
  if (!fb || !showMapControls()) return;

  if (canZoomIn()) {
    epd_fill_rect(kZoomInBounds.x, kZoomInBounds.y, kZoomInBounds.width, kZoomInBounds.height, AppConfig::kWhite, fb);
    epd_draw_rect(kZoomInBounds.x, kZoomInBounds.y, kZoomInBounds.width, kZoomInBounds.height, AppConfig::kBlack, fb);
    const int32_t cx = kZoomInBounds.x + kZoomInBounds.width / 2;
    const int32_t cy = kZoomInBounds.y + kZoomInBounds.height / 2;
    drawThickLine(fb, cx - 16, cy, cx + 16, cy, AppConfig::kBlack);
    drawThickLine(fb, cx, cy - 16, cx, cy + 16, AppConfig::kBlack);
  }
  if (canZoomOut()) {
    epd_fill_rect(kZoomOutBounds.x, kZoomOutBounds.y, kZoomOutBounds.width, kZoomOutBounds.height, AppConfig::kWhite, fb);
    epd_draw_rect(kZoomOutBounds.x, kZoomOutBounds.y, kZoomOutBounds.width, kZoomOutBounds.height, AppConfig::kBlack, fb);
    const int32_t cx = kZoomOutBounds.x + kZoomOutBounds.width / 2;
    const int32_t cy = kZoomOutBounds.y + kZoomOutBounds.height / 2;
    drawThickLine(fb, cx - 16, cy, cx + 16, cy, AppConfig::kBlack);
  }
}

void MapPage::drawPanButtons() {
  uint8_t* fb = framebuffer();
  if (!fb || !showMapControls()) return;

  auto drawPanButton = [fb](const Rect_t& bounds, uint8_t dir) {
    epd_fill_rect(bounds.x, bounds.y, bounds.width, bounds.height, AppConfig::kWhite, fb);
    epd_draw_rect(bounds.x, bounds.y, bounds.width, bounds.height, AppConfig::kBlack, fb);
    const int32_t cx = bounds.x + bounds.width / 2;
    const int32_t cy = bounds.y + bounds.height / 2;
    switch (dir) {
      case 0:
        epd_fill_triangle(cx, cy - 12, cx - 11, cy + 8, cx + 11, cy + 8, AppConfig::kBlack, fb);
        break;
      case 1:
        epd_fill_triangle(cx, cy + 12, cx - 11, cy - 8, cx + 11, cy - 8, AppConfig::kBlack, fb);
        break;
      case 2:
        epd_fill_triangle(cx - 12, cy, cx + 8, cy - 11, cx + 8, cy + 11, AppConfig::kBlack, fb);
        break;
      default:
        epd_fill_triangle(cx + 12, cy, cx - 8, cy - 11, cx - 8, cy + 11, AppConfig::kBlack, fb);
        break;
    }
  };

  drawPanButton(kPanUpBounds, 0);
  drawPanButton(kPanDownBounds, 1);
  drawPanButton(kPanLeftBounds, 2);
  drawPanButton(kPanRightBounds, 3);
}
