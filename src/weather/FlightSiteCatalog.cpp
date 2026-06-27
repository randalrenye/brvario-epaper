#include "weather/FlightSiteCatalog.h"

#include <ArduinoJson.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

static constexpr char kCatalogDirectory[] = "/weather";
static constexpr char kCatalogPath[] = "/weather/catalog.json";
static constexpr char kExpectedSchema[] = "brvario.flight-sites.catalog";
static constexpr uint16_t kSupportedSchemaVersion = 1;
static constexpr size_t kCatalogLineSize = 704;

// Minimal offline fallback. The full BRVARIO catalog lives in JSON and may
// contain only data whose redistribution is authorized.
static const FlightSite kFallbackSites[] = {
    {1, "PEDRA GRANDE", "ATIBAIA", "SP", -231687110, -465287090, 1382, 580, FlightWindN | FlightWindNE | FlightWindNW},
};
static constexpr uint16_t kFallbackCount = static_cast<uint16_t>(sizeof(kFallbackSites) / sizeof(kFallbackSites[0]));

fs::FS* catalogFs = nullptr;
uint32_t siteOffsets[FlightSiteCatalog::kMaxCatalogSites] = {};
uint16_t downloadedSiteCount = 0;
uint32_t downloadedCatalogVersion = 0;
char downloadedUpdatedAt[20] = {};
char catalogStatus[64] = "CATALOGO INTERNO";
bool downloadedCatalogActive = false;
FlightSite scratchSite = {};

bool readLine(File& file, char* line, size_t lineSize, bool& overflow) {
  overflow = false;
  if (!line || lineSize < 2 || !file.available()) {
    return false;
  }

  size_t length = 0;
  while (file.available()) {
    const int raw = file.read();
    if (raw < 0) {
      break;
    }
    const char value = static_cast<char>(raw);
    if (value == '\n') {
      break;
    }
    if (value == '\r') {
      continue;
    }
    if (length + 1 < lineSize) {
      line[length++] = value;
    } else {
      overflow = true;
    }
  }
  line[length] = '\0';
  return length > 0 || overflow;
}

const char* skipSpaces(const char* text) {
  while (text && *text != '\0' && isspace(static_cast<unsigned char>(*text))) {
    ++text;
  }
  return text;
}

bool isSiteLine(const char* line) {
  const char* text = skipSpaces(line);
  return text && strncmp(text, "{\"id\":", 6) == 0;
}

bool parseUnsignedField(const char* line, const char* field, uint32_t& value) {
  if (!line || !field) {
    return false;
  }
  const char* found = strstr(line, field);
  if (!found) {
    return false;
  }
  const char* colon = strchr(found, ':');
  if (!colon) {
    return false;
  }
  char* end = nullptr;
  const unsigned long parsed = strtoul(colon + 1, &end, 10);
  if (end == colon + 1) {
    return false;
  }
  value = static_cast<uint32_t>(parsed);
  return true;
}

bool parseQuotedField(const char* line, const char* field, char* output, size_t outputSize) {
  if (!line || !field || !output || outputSize == 0) {
    return false;
  }
  const char* found = strstr(line, field);
  if (!found) {
    return false;
  }
  const char* colon = strchr(found, ':');
  const char* firstQuote = colon ? strchr(colon + 1, '"') : nullptr;
  const char* lastQuote = firstQuote ? strchr(firstQuote + 1, '"') : nullptr;
  if (!firstQuote || !lastQuote) {
    return false;
  }
  const size_t length = static_cast<size_t>(lastQuote - firstQuote - 1);
  const size_t copyLength = length < outputSize - 1 ? length : outputSize - 1;
  memcpy(output, firstQuote + 1, copyLength);
  output[copyLength] = '\0';
  return true;
}

uint16_t quadrantBit(const char* text) {
  if (!text) return 0;
  if (strcmp(text, "N") == 0) return FlightWindN;
  if (strcmp(text, "NE") == 0) return FlightWindNE;
  if (strcmp(text, "E") == 0 || strcmp(text, "L") == 0) return FlightWindE;
  if (strcmp(text, "SE") == 0) return FlightWindSE;
  if (strcmp(text, "S") == 0) return FlightWindS;
  if (strcmp(text, "SW") == 0 || strcmp(text, "SO") == 0) return FlightWindSW;
  if (strcmp(text, "W") == 0 || strcmp(text, "O") == 0) return FlightWindW;
  if (strcmp(text, "NW") == 0 || strcmp(text, "NO") == 0) return FlightWindNW;
  return 0;
}

bool validSite(const FlightSite& site) {
  return site.id != 0 && site.name[0] != '\0' && site.city[0] != '\0' && strlen(site.state) == 2 &&
         site.latitudeE7 >= -900000000 && site.latitudeE7 <= 900000000 &&
         site.longitudeE7 >= -1800000000 && site.longitudeE7 <= 1800000000 &&
         (site.latitudeE7 != 0 || site.longitudeE7 != 0);
}

bool parseSiteLine(const char* line, FlightSite& site) {
  if (!isSiteLine(line)) {
    return false;
  }

  JsonDocument filter;
  filter["id"] = true;
  filter["name"] = true;
  filter["city"] = true;
  filter["state"] = true;
  filter["latitudeE7"] = true;
  filter["longitudeE7"] = true;
  filter["altitudeM"] = true;
  filter["verticalDropM"] = true;
  filter["windQuadrants"] = true;

  JsonDocument document;
  const DeserializationError error = deserializeJson(document, line, DeserializationOption::Filter(filter));
  if (error) {
    return false;
  }

  site = {};
  const uint32_t id = document["id"] | 0UL;
  if (id == 0 || id > 65535UL) {
    return false;
  }
  site.id = static_cast<uint16_t>(id);
  snprintf(site.name, sizeof(site.name), "%s", document["name"] | "");
  snprintf(site.city, sizeof(site.city), "%s", document["city"] | "");
  snprintf(site.state, sizeof(site.state), "%s", document["state"] | "");
  site.latitudeE7 = document["latitudeE7"] | 0;
  site.longitudeE7 = document["longitudeE7"] | 0;
  const int altitude = document["altitudeM"] | 0;
  if (altitude < -500 || altitude > 9000) {
    return false;
  }
  site.altitudeM = static_cast<int16_t>(altitude);
  const int verticalDrop = document["verticalDropM"] | 0;
  if (verticalDrop < 0 || verticalDrop > 9000) {
    return false;
  }
  site.verticalDropM = static_cast<int16_t>(verticalDrop);

  JsonVariantConst quadrants = document["windQuadrants"];
  if (quadrants.is<uint16_t>()) {
    site.windQuadrants = quadrants.as<uint16_t>();
  } else if (quadrants.is<JsonArrayConst>()) {
    for (const char* quadrant : quadrants.as<JsonArrayConst>()) {
      site.windQuadrants |= quadrantBit(quadrant);
    }
  }
  return validSite(site);
}

bool scanCatalog(fs::FS& filesystem,
                 const char* path,
                 uint32_t* offsets,
                 uint16_t& siteCount,
                 uint32_t& catalogVersion,
                 char* updatedAt,
                 size_t updatedAtSize,
                 char* errorText,
                 size_t errorTextSize) {
  siteCount = 0;
  catalogVersion = 0;
  if (updatedAt && updatedAtSize > 0) {
    updatedAt[0] = '\0';
  }

  File file = filesystem.open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    snprintf(errorText, errorTextSize, "ARQUIVO NAO ENCONTRADO");
    if (file) file.close();
    return false;
  }

  char line[kCatalogLineSize];
  char schema[48] = {};
  uint32_t schemaVersion = 0;
  uint32_t declaredSiteCount = 0;
  uint16_t seenIds[FlightSiteCatalog::kMaxCatalogSites] = {};
  bool overflow = false;
  while (file.available()) {
    const uint32_t lineOffset = static_cast<uint32_t>(file.position());
    if (!readLine(file, line, sizeof(line), overflow)) {
      continue;
    }
    if (overflow) {
      snprintf(errorText, errorTextSize, "ARQUIVO DE RAMPAS MUITO GRANDE");
      file.close();
      return false;
    }

    parseQuotedField(line, "\"schema\"", schema, sizeof(schema));
    parseUnsignedField(line, "\"schemaVersion\"", schemaVersion);
    parseUnsignedField(line, "\"catalogVersion\"", catalogVersion);
    parseUnsignedField(line, "\"siteCount\"", declaredSiteCount);
    if (updatedAt && updatedAtSize > 0) {
      parseQuotedField(line, "\"updatedAt\"", updatedAt, updatedAtSize);
    }

    if (!isSiteLine(line)) {
      continue;
    }
    if (siteCount >= FlightSiteCatalog::kMaxCatalogSites) {
      snprintf(errorText, errorTextSize, "CATALOGO EXCEDE %u RAMPAS", FlightSiteCatalog::kMaxCatalogSites);
      file.close();
      return false;
    }
    FlightSite parsed = {};
    if (!parseSiteLine(line, parsed)) {
      snprintf(errorText, errorTextSize, "DADOS DE RAMPA INVALIDOS");
      file.close();
      return false;
    }
    for (uint16_t i = 0; i < siteCount; ++i) {
      if (seenIds[i] == parsed.id) {
        snprintf(errorText, errorTextSize, "ID DE RAMPA DUPLICADO");
        file.close();
        return false;
      }
    }
    seenIds[siteCount] = parsed.id;
    if (offsets) {
      offsets[siteCount] = lineOffset;
    }
    ++siteCount;
  }
  file.close();

  if (strcmp(schema, kExpectedSchema) != 0 || schemaVersion != kSupportedSchemaVersion) {
    snprintf(errorText, errorTextSize, "FORMATO NAO SUPORTADO");
    return false;
  }
  if (catalogVersion == 0 || siteCount == 0) {
    snprintf(errorText, errorTextSize, "CATALOGO VAZIO/SEM VERSAO");
    return false;
  }
  if (declaredSiteCount != siteCount) {
    snprintf(errorText, errorTextSize, "CONTAGEM DE RAMPAS DIVERGENTE");
    return false;
  }
  if (updatedAt && updatedAtSize > 0 && updatedAt[0] == '\0') {
    snprintf(errorText, errorTextSize, "DATA DO CATALOGO AUSENTE");
    return false;
  }
  return true;
}

}  // namespace

bool FlightSiteCatalog::begin(fs::FS* filesystem) {
  catalogFs = filesystem;
  if (catalogFs && !catalogFs->exists(kCatalogDirectory)) {
    catalogFs->mkdir(kCatalogDirectory);
  }
  return reload();
}

bool FlightSiteCatalog::reload() {
  downloadedCatalogActive = false;
  downloadedSiteCount = 0;
  downloadedCatalogVersion = 0;
  downloadedUpdatedAt[0] = '\0';
  snprintf(catalogStatus, sizeof(catalogStatus), "CATALOGO INTERNO");
  if (!catalogFs) {
    return false;
  }

  char error[64] = {};
  if (!scanCatalog(*catalogFs,
                   kCatalogPath,
                   siteOffsets,
                   downloadedSiteCount,
                   downloadedCatalogVersion,
                   downloadedUpdatedAt,
                   sizeof(downloadedUpdatedAt),
                   error,
                   sizeof(error))) {
    snprintf(catalogStatus, sizeof(catalogStatus), "INTERNO: %.52s", error);
    downloadedSiteCount = 0;
    return false;
  }

  downloadedCatalogActive = true;
  snprintf(catalogStatus,
           sizeof(catalogStatus),
           "CATALOGO ATUAL - %u RAMPAS",
           static_cast<unsigned>(downloadedSiteCount));
  return true;
}

bool FlightSiteCatalog::validateFile(fs::FS& filesystem,
                                     const char* path,
                                     uint16_t* siteCount,
                                     uint32_t* catalogVersion,
                                     char* updatedAt,
                                     size_t updatedAtSize) {
  uint16_t count = 0;
  uint32_t version = 0;
  char error[64] = {};
  const bool valid = scanCatalog(filesystem, path, nullptr, count, version, updatedAt, updatedAtSize, error, sizeof(error));
  if (siteCount) *siteCount = count;
  if (catalogVersion) *catalogVersion = version;
  return valid;
}

uint16_t FlightSiteCatalog::count() {
  return downloadedCatalogActive ? downloadedSiteCount : kFallbackCount;
}

const FlightSite* FlightSiteCatalog::site(uint16_t index) {
  if (!downloadedCatalogActive) {
    return index < kFallbackCount ? &kFallbackSites[index] : nullptr;
  }
  if (!catalogFs || index >= downloadedSiteCount) {
    return nullptr;
  }

  File file = catalogFs->open(kCatalogPath, FILE_READ);
  if (!file || !file.seek(siteOffsets[index])) {
    if (file) file.close();
    return nullptr;
  }
  char line[kCatalogLineSize];
  bool overflow = false;
  const bool read = readLine(file, line, sizeof(line), overflow);
  file.close();
  if (!read || overflow || !parseSiteLine(line, scratchSite)) {
    return nullptr;
  }
  return &scratchSite;
}

const FlightSite* FlightSiteCatalog::findById(uint16_t id) {
  for (uint16_t i = 0; i < count(); ++i) {
    const FlightSite* candidate = site(i);
    if (candidate && candidate->id == id) {
      return candidate;
    }
  }
  return nullptr;
}

bool FlightSiteCatalog::usingDownloadedCatalog() {
  return downloadedCatalogActive;
}

uint32_t FlightSiteCatalog::catalogVersion() {
  return downloadedCatalogActive ? downloadedCatalogVersion : 0;
}

const char* FlightSiteCatalog::updatedAt() {
  return downloadedCatalogActive && downloadedUpdatedAt[0] != '\0' ? downloadedUpdatedAt : "--";
}

const char* FlightSiteCatalog::statusText() {
  return catalogStatus;
}

const char* FlightSiteCatalog::catalogPath() {
  return kCatalogPath;
}

void FlightSiteCatalog::formatWindQuadrants(uint16_t quadrants, char* text, size_t textSize) {
  if (!text || textSize == 0) {
    return;
  }

  text[0] = '\0';
  static const char* const kLabels[] = {"N", "NE", "L", "SE", "S", "SO", "O", "NO"};
  for (uint8_t i = 0; i < 8; ++i) {
    if ((quadrants & (1U << i)) == 0) {
      continue;
    }
    const size_t used = strlen(text);
    if (used + 1 >= textSize) {
      break;
    }
    snprintf(text + used, textSize - used, "%s%s", used == 0 ? "" : " / ", kLabels[i]);
  }

  if (text[0] == '\0') {
    snprintf(text, textSize, "--");
  }
}
