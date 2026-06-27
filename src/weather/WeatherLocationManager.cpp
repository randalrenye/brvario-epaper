#include "weather/WeatherLocationManager.h"

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

static constexpr char kPrefsNamespace[] = "weatherLoc";
static constexpr char kPrefsKey[] = "config";
static constexpr uint32_t kMagic = 0x4252574CUL;  // "BRWL"
static constexpr uint16_t kVersion = 2;

struct LegacyFlightSiteV1 {
  uint16_t id;
  char name[40];
  char city[28];
  char state[3];
  int32_t latitudeE7;
  int32_t longitudeE7;
  int16_t altitudeM;
  uint16_t windQuadrants;
};

struct StoredWeatherLocationsV1 {
  uint32_t magic;
  uint16_t version;
  uint8_t source;
  uint8_t favoriteCount;
  LegacyFlightSiteV1 selectedSite;
  LegacyFlightSiteV1 favorites[WeatherLocationManager::kMaxFavorites];
};

struct StoredWeatherLocations {
  uint32_t magic;
  uint16_t version;
  uint8_t source;
  uint8_t favoriteCount;
  FlightSite selectedSite;
  FlightSite favorites[WeatherLocationManager::kMaxFavorites];
};

bool sameSite(const FlightSite& a, const FlightSite& b) {
  if (a.id != 0 && b.id != 0) {
    return a.id == b.id;
  }
  return a.latitudeE7 == b.latitudeE7 && a.longitudeE7 == b.longitudeE7;
}

uint32_t mixHash(uint32_t hash, uint32_t value) {
  hash ^= value;
  hash *= 16777619UL;
  return hash;
}

FlightSite migrateLegacySite(const LegacyFlightSiteV1& legacy) {
  FlightSite site = {};
  site.id = legacy.id;
  memcpy(site.name, legacy.name, sizeof(site.name));
  memcpy(site.city, legacy.city, sizeof(site.city));
  memcpy(site.state, legacy.state, sizeof(site.state));
  site.latitudeE7 = legacy.latitudeE7;
  site.longitudeE7 = legacy.longitudeE7;
  site.altitudeM = legacy.altitudeM;
  site.verticalDropM = 0;
  site.windQuadrants = legacy.windQuadrants;
  return site;
}

}  // namespace

bool WeatherLocationManager::begin() {
  if (load()) {
    return true;
  }
  resetDefault();
  save();
  return false;
}

void WeatherLocationManager::updateGpsLocation(double lat, double lon, bool valid) {
  const int32_t latE7 = coordinateToE7(lat);
  const int32_t lonE7 = coordinateToE7(lon);
  gpsValid_ = valid && validCoordinatesE7(latE7, lonE7);
  if (gpsValid_) {
    gpsLatitudeE7_ = latE7;
    gpsLongitudeE7_ = lonE7;
  }
}

void WeatherLocationManager::useGpsLocation() {
  if (source_ == WeatherLocationSource::GpsCurrent) {
    return;
  }
  source_ = WeatherLocationSource::GpsCurrent;
  ++revision_;
  save();
}

bool WeatherLocationManager::selectSite(const FlightSite& site) {
  if (!validSite(site)) {
    return false;
  }
  selectedSite_ = site;
  normalizeSite(selectedSite_);
  source_ = WeatherLocationSource::FavoriteSite;
  ++revision_;
  return save();
}

bool WeatherLocationManager::setManualLocation(const char* name, double lat, double lon) {
  FlightSite manual = {};
  manual.id = 0;
  snprintf(manual.name, sizeof(manual.name), "%s", name && name[0] != '\0' ? name : "COORDENADA MANUAL");
  snprintf(manual.city, sizeof(manual.city), "LAT/LON INFORMADA");
  manual.latitudeE7 = coordinateToE7(lat);
  manual.longitudeE7 = coordinateToE7(lon);
  if (!validSite(manual)) {
    return false;
  }

  selectedSite_ = manual;
  normalizeSite(selectedSite_);
  source_ = WeatherLocationSource::ManualCoordinates;
  ++revision_;
  return save();
}

bool WeatherLocationManager::addFavorite(const FlightSite& site) {
  if (!validSite(site) || isFavorite(site) || favoriteCount_ >= kMaxFavorites) {
    return false;
  }
  favorites_[favoriteCount_] = site;
  normalizeSite(favorites_[favoriteCount_]);
  ++favoriteCount_;
  return save();
}

bool WeatherLocationManager::removeFavorite(uint8_t index) {
  if (index >= favoriteCount_) {
    return false;
  }
  for (uint8_t i = index; i + 1 < favoriteCount_; ++i) {
    favorites_[i] = favorites_[i + 1];
  }
  --favoriteCount_;
  favorites_[favoriteCount_] = {};
  return save();
}

bool WeatherLocationManager::hasValidLocation() const {
  if (source_ == WeatherLocationSource::GpsCurrent) {
    return gpsValid_ && validCoordinatesE7(gpsLatitudeE7_, gpsLongitudeE7_);
  }
  return validSite(selectedSite_);
}

double WeatherLocationManager::latitude() const {
  const int32_t value = source_ == WeatherLocationSource::GpsCurrent ? gpsLatitudeE7_ : selectedSite_.latitudeE7;
  return static_cast<double>(value) / 10000000.0;
}

double WeatherLocationManager::longitude() const {
  const int32_t value = source_ == WeatherLocationSource::GpsCurrent ? gpsLongitudeE7_ : selectedSite_.longitudeE7;
  return static_cast<double>(value) / 10000000.0;
}

const char* WeatherLocationManager::displayName() const {
  if (source_ == WeatherLocationSource::GpsCurrent) {
    return "GPS ATUAL";
  }
  return selectedSite_.name[0] != '\0' ? selectedSite_.name : "LOCAL SEM NOME";
}

const char* WeatherLocationManager::city() const {
  return source_ == WeatherLocationSource::GpsCurrent ? "POSICAO DO PILOTO" : selectedSite_.city;
}

const char* WeatherLocationManager::state() const {
  return source_ == WeatherLocationSource::GpsCurrent ? "" : selectedSite_.state;
}

int16_t WeatherLocationManager::altitudeM() const {
  return source_ == WeatherLocationSource::GpsCurrent ? 0 : selectedSite_.altitudeM;
}

int16_t WeatherLocationManager::verticalDropM() const {
  return source_ == WeatherLocationSource::GpsCurrent ? 0 : selectedSite_.verticalDropM;
}

uint16_t WeatherLocationManager::windQuadrants() const {
  return source_ == WeatherLocationSource::FavoriteSite ? selectedSite_.windQuadrants : 0;
}

const FlightSite* WeatherLocationManager::selectedSite() const {
  return source_ == WeatherLocationSource::GpsCurrent ? nullptr : &selectedSite_;
}

const char* WeatherLocationManager::sourceName() const {
  switch (source_) {
    case WeatherLocationSource::GpsCurrent:
      return "GPS ATUAL";
    case WeatherLocationSource::FavoriteSite:
      return isFavorite(selectedSite_) ? "RAMPA FAVORITA" : "CATALOGO DE RAMPAS";
    case WeatherLocationSource::ManualCoordinates:
      return "COORDENADA MANUAL";
  }
  return "LOCAL DESCONHECIDO";
}

uint32_t WeatherLocationManager::locationKey() const {
  if (source_ == WeatherLocationSource::GpsCurrent) {
    return 0x47505301UL;
  }
  if (source_ == WeatherLocationSource::FavoriteSite && selectedSite_.id != 0) {
    return 0x53000000UL | static_cast<uint32_t>(selectedSite_.id);
  }

  uint32_t hash = 2166136261UL;
  hash = mixHash(hash, static_cast<uint32_t>(selectedSite_.latitudeE7));
  hash = mixHash(hash, static_cast<uint32_t>(selectedSite_.longitudeE7));
  return hash ^ 0x4D000000UL;
}

const FlightSite* WeatherLocationManager::favorite(uint8_t index) const {
  return index < favoriteCount_ ? &favorites_[index] : nullptr;
}

bool WeatherLocationManager::isFavorite(const FlightSite& site) const {
  for (uint8_t i = 0; i < favoriteCount_; ++i) {
    if (sameSite(favorites_[i], site)) {
      return true;
    }
  }
  return false;
}

bool WeatherLocationManager::load() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    return false;
  }
  const size_t storedSize = prefs.getBytesLength(kPrefsKey);
  if (storedSize == sizeof(StoredWeatherLocationsV1)) {
    StoredWeatherLocationsV1 legacy = {};
    const size_t read = prefs.getBytes(kPrefsKey, &legacy, sizeof(legacy));
    prefs.end();
    if (read != sizeof(legacy) || legacy.magic != kMagic || legacy.version != 1 ||
        legacy.source > static_cast<uint8_t>(WeatherLocationSource::ManualCoordinates) ||
        legacy.favoriteCount > kMaxFavorites) {
      return false;
    }
    source_ = static_cast<WeatherLocationSource>(legacy.source);
    selectedSite_ = migrateLegacySite(legacy.selectedSite);
    favoriteCount_ = legacy.favoriteCount;
    for (uint8_t i = 0; i < kMaxFavorites; ++i) {
      favorites_[i] = migrateLegacySite(legacy.favorites[i]);
      normalizeSite(favorites_[i]);
    }
    normalizeSite(selectedSite_);
    if (source_ != WeatherLocationSource::GpsCurrent && !validSite(selectedSite_)) {
      return false;
    }
    for (uint8_t i = 0; i < favoriteCount_; ++i) {
      if (!validSite(favorites_[i])) {
        return false;
      }
    }
    revision_ = 1;
    save();
    return true;
  }
  if (storedSize != sizeof(StoredWeatherLocations)) {
    prefs.end();
    return false;
  }

  StoredWeatherLocations stored = {};
  const size_t read = prefs.getBytes(kPrefsKey, &stored, sizeof(stored));
  prefs.end();
  if (read != sizeof(stored) || stored.magic != kMagic || stored.version != kVersion ||
      stored.source > static_cast<uint8_t>(WeatherLocationSource::ManualCoordinates) || stored.favoriteCount > kMaxFavorites) {
    return false;
  }

  for (uint8_t i = 0; i < stored.favoriteCount; ++i) {
    if (!validSite(stored.favorites[i])) {
      return false;
    }
  }
  const WeatherLocationSource loadedSource = static_cast<WeatherLocationSource>(stored.source);
  if (loadedSource != WeatherLocationSource::GpsCurrent && !validSite(stored.selectedSite)) {
    return false;
  }

  source_ = loadedSource;
  selectedSite_ = stored.selectedSite;
  normalizeSite(selectedSite_);
  favoriteCount_ = stored.favoriteCount;
  for (uint8_t i = 0; i < kMaxFavorites; ++i) {
    favorites_[i] = stored.favorites[i];
    normalizeSite(favorites_[i]);
  }
  revision_ = 1;
  return true;
}

bool WeatherLocationManager::save() const {
  StoredWeatherLocations stored = {};
  stored.magic = kMagic;
  stored.version = kVersion;
  stored.source = static_cast<uint8_t>(source_);
  stored.favoriteCount = favoriteCount_;
  stored.selectedSite = selectedSite_;
  for (uint8_t i = 0; i < kMaxFavorites; ++i) {
    stored.favorites[i] = favorites_[i];
  }

  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    Serial.println("Local meteo: falha ao abrir NVS.");
    return false;
  }
  const size_t written = prefs.putBytes(kPrefsKey, &stored, sizeof(stored));
  prefs.end();
  if (written != sizeof(stored)) {
    Serial.println("Local meteo: falha ao salvar NVS.");
    return false;
  }
  return true;
}

void WeatherLocationManager::resetDefault() {
  source_ = WeatherLocationSource::GpsCurrent;
  selectedSite_ = {};
  for (uint8_t i = 0; i < kMaxFavorites; ++i) {
    favorites_[i] = {};
  }
  favoriteCount_ = 0;
  revision_ = 1;
}

bool WeatherLocationManager::validCoordinatesE7(int32_t latE7, int32_t lonE7) {
  if (latE7 < -900000000 || latE7 > 900000000 || lonE7 < -1800000000 || lonE7 > 1800000000) {
    return false;
  }
  return latE7 != 0 || lonE7 != 0;
}

bool WeatherLocationManager::validSite(const FlightSite& site) {
  return site.name[0] != '\0' && validCoordinatesE7(site.latitudeE7, site.longitudeE7);
}

int32_t WeatherLocationManager::coordinateToE7(double value) {
  if (!isfinite(value) || value < -180.0 || value > 180.0) {
    return 0;
  }
  return static_cast<int32_t>(llround(value * 10000000.0));
}

void WeatherLocationManager::normalizeSite(FlightSite& site) {
  site.name[sizeof(site.name) - 1] = '\0';
  site.city[sizeof(site.city) - 1] = '\0';
  site.state[sizeof(site.state) - 1] = '\0';
}
