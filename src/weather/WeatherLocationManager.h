#pragma once

#include <stdint.h>

#include "weather/FlightSiteCatalog.h"

enum class WeatherLocationSource : uint8_t {
  GpsCurrent,
  FavoriteSite,
  ManualCoordinates,
};

class WeatherLocationManager {
 public:
  static constexpr uint8_t kMaxFavorites = 5;

  bool begin();
  void updateGpsLocation(double lat, double lon, bool valid);
  void useGpsLocation();
  bool selectSite(const FlightSite& site);
  bool setManualLocation(const char* name, double lat, double lon);
  bool addFavorite(const FlightSite& site);
  bool removeFavorite(uint8_t index);

  WeatherLocationSource source() const { return source_; }
  bool hasValidLocation() const;
  double latitude() const;
  double longitude() const;
  const char* displayName() const;
  const char* city() const;
  const char* state() const;
  int16_t altitudeM() const;
  int16_t verticalDropM() const;
  uint16_t windQuadrants() const;
  const FlightSite* selectedSite() const;
  const char* sourceName() const;
  uint32_t locationKey() const;
  uint32_t revision() const { return revision_; }

  uint8_t favoriteCount() const { return favoriteCount_; }
  const FlightSite* favorite(uint8_t index) const;
  bool isFavorite(const FlightSite& site) const;

 private:
  WeatherLocationSource source_ = WeatherLocationSource::GpsCurrent;
  FlightSite selectedSite_ = {};
  FlightSite favorites_[kMaxFavorites] = {};
  uint8_t favoriteCount_ = 0;
  int32_t gpsLatitudeE7_ = 0;
  int32_t gpsLongitudeE7_ = 0;
  bool gpsValid_ = false;
  uint32_t revision_ = 1;

  bool load();
  bool save() const;
  void resetDefault();
  static bool validCoordinatesE7(int32_t latE7, int32_t lonE7);
  static bool validSite(const FlightSite& site);
  static int32_t coordinateToE7(double value);
  static void normalizeSite(FlightSite& site);
};
