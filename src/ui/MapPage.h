#pragma once

#include <stddef.h>
#include <stdint.h>

#include "data/VarioData.h"
#include "display/EpdDisplay.h"

class StorageManager;

class MapPage {
 public:
  void begin();
  void begin(EpdDisplay& display);
  void attachStorageManager(StorageManager* storage);
  void draw();
  void drawBase();
  void drawDynamic();
  void updateMockData();

  void setPosition(float lat, float lon);
  void setHeading(float headingDeg);
  void setWind(float windToDirDeg, float windSpeedKmh);
  void clearWind();
  void setFlightData(float altitudeM,
                     float altitudeAglM,
                     float groundSpeedKmh,
                     float varioMs,
                     float glideRatio,
                     float thermalGainM,
                     uint32_t elapsedSeconds);
  void setSystemStatus(uint8_t satellites, uint8_t batteryPercent, bool gpsFix, bool trackingEnabled, uint32_t timeOfDaySeconds);
  void setThermalOverlay(const ThermalAssistPoint* points,
                         uint8_t count,
                         const ThermalHistoryPoint* history,
                         uint8_t historyCount,
                         float pilotEastM,
                         float pilotNorthM,
                         uint8_t coreConfidence);
  void panByMeters(float eastM, float northM);
  void panByScreenStep(int8_t eastSteps, int8_t northSteps);
  void zoomIn();
  void zoomOut();
  bool canZoomIn() const;
  bool canZoomOut() const;
  bool showMapControls() const { return !trackingEnabled_; }
  bool showPanControls() const { return showMapControls(); }
  bool needsBaseRedraw() const { return baseDirty_; }
  bool needsDynamicRedraw() const { return dynamicDirty_; }
  void clearBaseDirty() { baseDirty_ = false; }
  void clearDynamicDirty() { dynamicDirty_ = false; }
  bool hasPreparedBaseCache() const;
  bool restorePreparedBaseCache();
  void savePreparedBaseCache();
  void invalidatePreparedBaseCache();
  Rect_t mapBounds() const;
  Rect_t dynamicBounds() const;

 private:
  static constexpr uint16_t kMaxTrailPoints = 320;
  static constexpr uint8_t kMaxThermalBubbles = 16;
  static constexpr uint8_t kMaxThermalHistoryBubbles = kThermalHistoryPoints;

  struct TrailPoint {
    float lat;
    float lon;
    uint32_t timestampMs;
    bool valid;
  };

  struct ThermalBubble {
    float eastM;
    float northM;
    float liftMs;
    bool valid;
  };

  struct ThermalHistoryBubble {
    float eastM;
    float northM;
    float coreMs;
    uint8_t confidencePercent;
    uint8_t ageMinutes;
    bool active;
    bool valid;
  };

  struct PreparedBaseCache {
    uint8_t* buffer = nullptr;
    size_t size = 0;
    int32_t latE7 = 0;
    int32_t lonE7 = 0;
    uint8_t zoom = 255;
    bool trackingEnabled = false;
    bool valid = false;
  };

  EpdDisplay* display_ = nullptr;
  StorageManager* storage_ = nullptr;

  float currentLat = -16.7360F;
  float currentLon = -40.4290F;
  float mapCenterLat = -16.7360F;
  float mapCenterLon = -40.4290F;
  uint8_t zoomLevel_ = 4;
  bool mapCenterValid_ = false;
  bool baseDirty_ = true;
  bool dynamicDirty_ = true;
  static constexpr uint8_t kPreparedBaseCacheSlots = 3;
  PreparedBaseCache preparedBaseCaches_[kPreparedBaseCacheSlots] = {};
  uint32_t manualPanHoldUntilMs_ = 0;

  TrailPoint trail_[kMaxTrailPoints] = {};
  uint16_t trailHead_ = 0;
  uint16_t trailCount_ = 0;

  float headingDeg = 45.0F;
  float windFromDirDeg = 70.0F;
  float windSpeedKmh = 18.0F;
  bool windAvailable_ = false;

  int altitude = 850;
  int altitudeAgl = 0;
  float vario = 1.8F;
  float groundSpeed = 42.0F;
  float glideRatio = 0.0F;
  float thermalGainM = 0.0F;
  uint32_t elapsedSeconds = 0;
  uint8_t satellites_ = 0;
  uint8_t batteryPercent_ = 100;
  uint32_t timeOfDaySeconds_ = 0;
  bool gpsFix_ = false;
  bool trackingEnabled_ = false;
  ThermalBubble thermalBubbles_[kMaxThermalBubbles] = {};
  uint8_t thermalBubbleCount_ = 0;
  ThermalHistoryBubble thermalHistory_[kMaxThermalHistoryBubbles] = {};
  uint8_t thermalHistoryCount_ = 0;
  uint8_t thermalCoreConfidence_ = 0;

  void markBaseDirty();
  void markDynamicDirty();
  bool ensurePreparedBaseCache(uint8_t slot);
  bool preparedBaseMatchesCurrentView(uint8_t slot) const;
  void loadPreferredZoom();
  void savePreferredZoom() const;
  float distanceFromCurrentM(float lat, float lon) const;
  uint8_t* framebuffer() const;
  void drawText(const char* text, int32_t x, int32_t baselineY);
  void drawCenteredText(const char* text, int32_t centerX, int32_t baselineY);
  void drawSmallValue(const char* label, const char* value, const char* unit, int32_t x, int32_t y, int32_t w);
  void drawArrow(int32_t cx, int32_t cy, float bearingDeg, int32_t length, uint8_t color, bool filledHead);

  void ensureMapCenter();
  void addTrailPoint(float lat, float lon);
  bool projectLatLon(float lat, float lon, int32_t& x, int32_t& y) const;
  float metersPerPixel() const;
  bool pointInsideMap(int32_t x, int32_t y, int32_t margin) const;
  void drawMapArea();
  bool drawOfflineMapVectors();
  void drawFlightTrail();
  void drawWindDirectionOverlay();
  void drawPilotMarker();
  void drawTopInfoBar();
  void drawBatteryIndicator();
  void drawVarioBar();
  void drawThermalOverlay();
  void drawScaleBar();
  void drawFlightDuration();
  void drawGlideRatio();
  void drawZoomButtons();
  void drawPanButtons();
  void drawOfflineMapPlaceholder();
};
