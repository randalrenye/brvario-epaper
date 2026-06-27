#include "storage/StorageManager.h"

#include <SD.h>
#include <SPI.h>
#include <stdio.h>
#include <string.h>

#include "utilities.h"

namespace {

static constexpr uint32_t kSdFrequencyHz = 10000000UL;
static constexpr char kMapsRoot[] = "/maps";
static constexpr char kOpenTopoRoot[] = "/maps/opentopo";
static constexpr char kRegionsRoot[] = "/maps/regions";

const char* cardTypeName(uint8_t type) {
  switch (type) {
    case CARD_MMC:
      return "MMC";
    case CARD_SD:
      return "SD";
    case CARD_SDHC:
      return "SDHC";
    case CARD_NONE:
    default:
      return "---";
  }
}

}  // namespace

bool StorageManager::begin() {
  return refresh();
}

bool StorageManager::refresh() {
  if (!mountCard()) {
    mounted_ = false;
    mapsReady_ = false;
    cardSizeBytes_ = 0;
    totalBytes_ = 0;
    usedBytes_ = 0;
    mapsBytes_ = 0;
    mapFileCount_ = 0;
    snprintf(cardTypeText_, sizeof(cardTypeText_), "---");
    return false;
  }

  updateStats();
  ensureMapDirectories();
  updateStats();
  return true;
}

bool StorageManager::ensureMounted() {
  if (mounted_ && SD.cardType() != CARD_NONE) {
    return true;
  }

  mounted_ = false;
  mapsReady_ = false;
  if (!mountCard()) {
    return false;
  }
  ensureMapDirectories();
  return true;
}

void StorageManager::end() {
  SD.end();
  mounted_ = false;
  mapsReady_ = false;
  setStatus("SD DESMONTADO");
}

fs::FS* StorageManager::filesystem() {
  return mounted_ ? static_cast<fs::FS*>(&SD) : nullptr;
}

bool StorageManager::ensureMapDirectories() {
  if (!mounted_ && !mountCard()) {
    return false;
  }

  const bool ok = ensureDir(kMapsRoot) && ensureDir(kOpenTopoRoot) && ensureDir(kRegionsRoot);
  mapsReady_ = ok;
  setStatus(ok ? "PASTAS DE MAPAS OK" : "FALHA AO CRIAR PASTAS");
  return ok;
}

bool StorageManager::clearMaps() {
  if (!mounted_ && !mountCard()) {
    return false;
  }

  removeRecursive(kMapsRoot);
  const bool ok = ensureMapDirectories();
  updateStats();
  setStatus(ok ? "MAPAS APAGADOS" : "FALHA AO RECRIAR /maps");
  return ok;
}

bool StorageManager::hasRegionPackage(const char* regionName) const {
  if (!mounted_ || !regionName || regionName[0] == '\0') {
    return false;
  }

  char path[96];
  snprintf(path, sizeof(path), "%s/%s.brmap", kRegionsRoot, regionName);
  if (SD.exists(path)) return true;
  snprintf(path, sizeof(path), "%s/%s.brvario", kRegionsRoot, regionName);
  if (SD.exists(path)) return true;
  snprintf(path, sizeof(path), "%s/%s.bin", kRegionsRoot, regionName);
  return SD.exists(path);
}

bool StorageManager::mountCard() {
  if (mounted_ && SD.cardType() != CARD_NONE) {
    return true;
  }

  SD.end();
  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI, kSdFrequencyHz, "/sd", 8, false)) {
    setStatus("SD NAO MONTADO");
    return false;
  }

  const uint8_t type = SD.cardType();
  snprintf(cardTypeText_, sizeof(cardTypeText_), "%s", cardTypeName(type));
  if (type == CARD_NONE) {
    SD.end();
    setStatus("SEM CARTAO SD");
    return false;
  }

  mounted_ = true;
  setStatus("SD MONTADO");
  return true;
}

void StorageManager::updateStats() {
  if (!mounted_) {
    return;
  }

  cardSizeBytes_ = SD.cardSize();
  totalBytes_ = SD.totalBytes();
  usedBytes_ = SD.usedBytes();
  mapsBytes_ = 0;
  mapFileCount_ = 0;
  if (SD.exists(kMapsRoot)) {
    scanDirectory(kMapsRoot, mapsBytes_, mapFileCount_);
  }
  mapsReady_ = SD.exists(kMapsRoot);
}

void StorageManager::setStatus(const char* text) {
  snprintf(statusText_, sizeof(statusText_), "%s", text ? text : "");
}

bool StorageManager::ensureDir(const char* path) {
  if (SD.exists(path)) {
    File dir = SD.open(path);
    const bool isDir = dir && dir.isDirectory();
    dir.close();
    return isDir;
  }
  return SD.mkdir(path);
}

bool StorageManager::removeRecursive(const char* path) {
  File root = SD.open(path);
  if (!root) {
    return true;
  }

  if (!root.isDirectory()) {
    root.close();
    return SD.remove(path);
  }

  File entry = root.openNextFile();
  while (entry) {
    char childPath[128];
    const char* entryName = entry.name();
    if (entryName && entryName[0] == '/') {
      snprintf(childPath, sizeof(childPath), "%s", entryName);
    } else {
      snprintf(childPath, sizeof(childPath), "%s/%s", path, entryName ? entryName : "");
    }
    const bool isDir = entry.isDirectory();
    entry.close();
    if (isDir) {
      removeRecursive(childPath);
    } else {
      SD.remove(childPath);
    }
    entry = root.openNextFile();
  }
  root.close();
  return SD.rmdir(path);
}

void StorageManager::scanDirectory(const char* path, uint64_t& bytes, uint16_t& files) const {
  File root = SD.open(path);
  if (!root) {
    return;
  }

  if (!root.isDirectory()) {
    bytes += root.size();
    ++files;
    root.close();
    return;
  }

  File entry = root.openNextFile();
  while (entry) {
    char childPath[128];
    const char* entryName = entry.name();
    if (entryName && entryName[0] == '/') {
      snprintf(childPath, sizeof(childPath), "%s", entryName);
    } else {
      snprintf(childPath, sizeof(childPath), "%s/%s", path, entryName ? entryName : "");
    }
    const bool isDir = entry.isDirectory();
    const size_t size = entry.size();
    entry.close();
    if (isDir) {
      scanDirectory(childPath, bytes, files);
    } else {
      bytes += size;
      ++files;
    }
    entry = root.openNextFile();
  }
  root.close();
}
