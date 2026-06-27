#pragma once

#include <stddef.h>
#include <stdint.h>

#include <FS.h>

struct FlightSite {
  uint16_t id;
  char name[40];
  char city[28];
  char state[3];
  int32_t latitudeE7;
  int32_t longitudeE7;
  int16_t altitudeM;
  int16_t verticalDropM;
  uint16_t windQuadrants;
};

enum FlightWindQuadrant : uint16_t {
  FlightWindN = 1U << 0,
  FlightWindNE = 1U << 1,
  FlightWindE = 1U << 2,
  FlightWindSE = 1U << 3,
  FlightWindS = 1U << 4,
  FlightWindSW = 1U << 5,
  FlightWindW = 1U << 6,
  FlightWindNW = 1U << 7,
};

class FlightSiteCatalog {
 public:
  static constexpr uint16_t kMaxCatalogSites = 512;

  static bool begin(fs::FS* filesystem);
  static bool reload();
  static bool validateFile(fs::FS& filesystem,
                           const char* path,
                           uint16_t* siteCount = nullptr,
                           uint32_t* catalogVersion = nullptr,
                           char* updatedAt = nullptr,
                           size_t updatedAtSize = 0);

  static uint16_t count();
  static const FlightSite* site(uint16_t index);
  static const FlightSite* findById(uint16_t id);
  static bool usingDownloadedCatalog();
  static uint32_t catalogVersion();
  static const char* updatedAt();
  static const char* statusText();
  static const char* catalogPath();
  static void formatWindQuadrants(uint16_t quadrants, char* text, size_t textSize);
};
