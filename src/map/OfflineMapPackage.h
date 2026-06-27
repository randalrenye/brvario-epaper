#pragma once

#include <Arduino.h>
#include <FS.h>
#include <stdint.h>

#include "epd_driver.h"

class EpdDisplay;

enum class OfflineMapFeatureType : uint8_t {
  Contour = 1,
  River = 2,
  Road = 3,
  Trail = 4,
  City = 5,
  Ramp = 6,
  Waypoint = 7,
  IndexContour = 8,
};

struct OfflineMapRenderStats {
  bool foundPackage = false;
  bool hasNearestWaypoint = false;
  char packageName[32] = "";
  char nearestWaypointName[24] = "";
  float nearestWaypointDistanceKm = 0.0F;
  float nearestWaypointBearingDeg = 0.0F;
  uint16_t linesDrawn = 0;
  uint16_t pointsDrawn = 0;
  uint16_t labelsDrawn = 0;
};

class OfflineMapPackage {
 public:
  struct View {
    float centerLat = 0.0F;
    float centerLon = 0.0F;
    float metersPerPixel = 8.0F;
    int32_t pilotX = 0;
    int32_t pilotY = 0;
    Rect_t bounds = {0, 0, 0, 0};
  };

  static bool renderCovering(fs::FS& fs, EpdDisplay& display, const View& view, OfflineMapRenderStats& stats);
  static void invalidateCache();

 private:
  static bool renderFile(fs::FS& fs, const char* path, EpdDisplay& display, const View& view, OfflineMapRenderStats& stats);
};
