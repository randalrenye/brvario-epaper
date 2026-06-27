#include "network/MapDownloadManager.h"

#include <math.h>
#include <WiFi.h>
#include <stdio.h>
#include <string.h>

#include "config/MapDownloadConfig.h"
#include "network/BrazilMapCatalog.h"
#include "network/WifiManager.h"
#include "storage/StorageManager.h"

namespace {

static constexpr float kDegToRad = 0.01745329251994329577F;
static constexpr float kEarthRadiusKm = 6371.0F;
static constexpr uint16_t kStatePageSize = 5;

static constexpr MapRegionInfo kNoLegacyRegion = {"", "---", "", 0.0F, 0.0F, 0};

bool startsWithHttps(const char* url) {
  return url && strncmp(url, "https://", 8) == 0;
}

const char* targetKindText(MapDownloadTargetKind kind) {
  switch (kind) {
    case MapDownloadTargetKind::CurrentRegion:
      return "current";
    case MapDownloadTargetKind::MacroRegion:
      return "macro";
    case MapDownloadTargetKind::State:
      return "state";
    case MapDownloadTargetKind::StateBundle:
      return "state_bundle";
    case MapDownloadTargetKind::Package:
      return "package";
    case MapDownloadTargetKind::PagePrev:
      return "prev";
    case MapDownloadTargetKind::PageNext:
      return "next";
    case MapDownloadTargetKind::DownloadedMaps:
      return "downloaded";
    case MapDownloadTargetKind::DeleteMaps:
      return "delete";
  }
  return "unknown";
}

float distanceKm(float latA, float lonA, float latB, float lonB) {
  const float phi1 = latA * kDegToRad;
  const float phi2 = latB * kDegToRad;
  const float dPhi = (latB - latA) * kDegToRad;
  const float dLambda = (lonB - lonA) * kDegToRad;
  const float s1 = sinf(dPhi * 0.5F);
  const float s2 = sinf(dLambda * 0.5F);
  const float a = s1 * s1 + cosf(phi1) * cosf(phi2) * s2 * s2;
  const float c = 2.0F * atan2f(sqrtf(a), sqrtf(fmaxf(0.0F, 1.0F - a)));
  return kEarthRadiusKm * c;
}

bool validCoordinate(float lat, float lon) {
  return isfinite(lat) && isfinite(lon) && fabsf(lat) <= 90.0F && fabsf(lon) <= 180.0F && (fabsf(lat) > 0.0001F || fabsf(lon) > 0.0001F);
}

int32_t coordToE7(float value) {
  return static_cast<int32_t>(lroundf(value * 10000000.0F));
}

float e7ToDeg(int32_t value) {
  return static_cast<float>(value) / 10000000.0F;
}

bool pointInPackage(const BrazilMapPackageInfo& package, int32_t latE7, int32_t lonE7) {
  return latE7 >= package.latMinE7 && latE7 <= package.latMaxE7 && lonE7 >= package.lonMinE7 && lonE7 <= package.lonMaxE7;
}

bool packageIntersects(const BrazilMapPackageInfo& a, const BrazilMapPackageInfo& b) {
  return a.latMinE7 <= b.latMaxE7 && a.latMaxE7 >= b.latMinE7 && a.lonMinE7 <= b.lonMaxE7 && a.lonMaxE7 >= b.lonMinE7;
}

float packageCenterLat(const BrazilMapPackageInfo& package) {
  return e7ToDeg((package.latMinE7 + package.latMaxE7) / 2);
}

float packageCenterLon(const BrazilMapPackageInfo& package) {
  return e7ToDeg((package.lonMinE7 + package.lonMaxE7) / 2);
}

char lowerAscii(char value) {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

void packageId(uint16_t packageIndex, char* out, size_t outSize) {
  if (packageIndex >= kBrazilMapPackageCount) {
    snprintf(out, outSize, "mapa");
    return;
  }
  const BrazilMapPackageInfo& package = kBrazilMapPackages[packageIndex];
  const BrazilStateInfo& state = kBrazilStates[package.stateIndex];
  snprintf(out, outSize, "%c%c_%03u", lowerAscii(state.uf[0]), lowerAscii(state.uf[1]), static_cast<unsigned>(package.number));
}

void packageName(uint16_t packageIndex, char* out, size_t outSize) {
  if (packageIndex >= kBrazilMapPackageCount) {
    snprintf(out, outSize, "MAPA");
    return;
  }
  const BrazilMapPackageInfo& package = kBrazilMapPackages[packageIndex];
  const BrazilStateInfo& state = kBrazilStates[package.stateIndex];
  snprintf(out, outSize, "%s %03u", state.uf, static_cast<unsigned>(package.number));
}

}  // namespace

void MapDownloadManager::begin(StorageManager* storage, WifiManager* wifi) {
  storage_ = storage;
  wifi_ = wifi;
  view_ = MapDownloadView::MacroRegions;
  selectedTarget_ = 0;
  selectedMacroIndex_ = 3;  // Sudeste is the first generated region set.
  selectedStateIndex_ = 21; // MG default while the first public map set is being built.
  browserPage_ = 0;
  resetStatus();
  refreshOptions();
}

void MapDownloadManager::update() {
  if (state_ == MapDownloadState::WaitingWifi) {
    startWhenWifiReady();
    return;
  }

  if (state_ != MapDownloadState::Downloading) {
    return;
  }

  WiFiClient* stream = http_.getStreamPtr();
  if (!stream) {
    fail("STREAM HTTP INVALIDO");
    return;
  }

  uint16_t budget = sizeof(chunk_) * 4;
  while (stream->available() > 0 && budget > 0) {
    const int available = stream->available();
    const size_t toRead = available > static_cast<int>(sizeof(chunk_)) ? sizeof(chunk_) : static_cast<size_t>(available);
    const int readBytes = stream->read(chunk_, toRead);
    if (readBytes <= 0) {
      break;
    }
    const size_t written = output_.write(chunk_, static_cast<size_t>(readBytes));
    if (written != static_cast<size_t>(readBytes)) {
      fail("FALHA AO GRAVAR SD");
      return;
    }
    downloadedBytes_ += static_cast<uint32_t>(readBytes);
    lastDataMs_ = millis();
    budget = budget > static_cast<uint16_t>(readBytes) ? budget - static_cast<uint16_t>(readBytes) : 0;
    if (contentLength_ > 0) {
      const uint32_t pct = (downloadedBytes_ * 100UL) / static_cast<uint32_t>(contentLength_);
      progressPercent_ = pct > 100UL ? 100 : static_cast<uint8_t>(pct);
    }
  }

  if (contentLength_ > 0 && downloadedBytes_ >= static_cast<uint32_t>(contentLength_)) {
    finishDownload();
    return;
  }

  if (!http_.connected() && stream->available() == 0) {
    if (contentLength_ <= 0 && downloadedBytes_ > 0) {
      finishDownload();
    } else {
      fail("DOWNLOAD INCOMPLETO");
    }
    return;
  }

  if (millis() - lastDataMs_ > MapDownloadConfig::kNoDataTimeoutMs) {
    fail("TIMEOUT DO DOWNLOAD");
  }
}

uint8_t MapDownloadManager::targetCount() const {
  return optionCount_;
}

const MapDownloadTargetInfo& MapDownloadManager::target(uint8_t index) const {
  return options_[index < optionCount_ ? index : 0];
}

const char* MapDownloadManager::titleText() const {
  switch (view_) {
    case MapDownloadView::MacroRegions:
      return "MAPAS OFFLINE";
    case MapDownloadView::States:
      return "ESCOLHA O ESTADO";
    case MapDownloadView::Packages:
      return "MAPAS DO ESTADO";
  }
  return "MAPAS OFFLINE";
}

const char* MapDownloadManager::subtitleText() const {
  switch (view_) {
    case MapDownloadView::MacroRegions:
      return "Brasil por regioes, depois estado e pacote regional";
    case MapDownloadView::States:
      return kBrazilMacroRegions[selectedMacroIndex_].name;
    case MapDownloadView::Packages:
      return "Baixe tudo ou use o GPS para baixar perto de voce";
  }
  return "";
}

void MapDownloadManager::selectTarget(uint8_t index) {
  if (index >= optionCount_ || busy()) {
    return;
  }

  const MapDownloadTargetInfo& info = options_[index];
  if (info.kind == MapDownloadTargetKind::PageNext) {
    ++browserPage_;
    refreshOptions();
    setStatus("PROXIMA PAGINA");
    return;
  }
  if (info.kind == MapDownloadTargetKind::PagePrev) {
    if (browserPage_ > 0) {
      --browserPage_;
    }
    refreshOptions();
    setStatus("PAGINA ANTERIOR");
    return;
  }

  selectedTarget_ = index;
  deleteConfirm_ = false;
  if (info.kind == MapDownloadTargetKind::CurrentRegion) {
    setStatus("BAIXAR PELO GPS");
  } else if (info.kind == MapDownloadTargetKind::MacroRegion || info.kind == MapDownloadTargetKind::State) {
    setStatus("TOQUE ENTRAR");
  } else if (info.kind == MapDownloadTargetKind::StateBundle) {
    setStatus("BAIXAR ESTADO TODO");
  } else if (info.kind == MapDownloadTargetKind::Package) {
    setStatus(targetDownloaded(index) ? "MAPA JA NO SD" : "PACOTE SELECIONADO");
  } else if (info.kind == MapDownloadTargetKind::DeleteMaps) {
    setStatus("EXCLUIR: TOQUE APAGAR");
  } else {
    setStatus("SELECIONADO");
  }
}

void MapDownloadManager::goBack() {
  if (busy()) {
    return;
  }
  if (view_ == MapDownloadView::Packages) {
    view_ = MapDownloadView::States;
    browserPage_ = 0;
    refreshOptions();
    setStatus("ESCOLHA ESTADO");
  } else if (view_ == MapDownloadView::States) {
    view_ = MapDownloadView::MacroRegions;
    browserPage_ = 0;
    refreshOptions();
    setStatus("ESCOLHA REGIAO");
  }
}

bool MapDownloadManager::targetDownloaded(uint8_t index) const {
  if (!storage_ || index >= optionCount_) {
    return false;
  }

  const MapDownloadTargetInfo& info = options_[index];
  if (info.kind == MapDownloadTargetKind::DownloadedMaps) {
    return storage_->mapFileCount() > 0;
  }
  if (info.kind == MapDownloadTargetKind::Package) {
    return targetFileExists(info.fileName);
  }
  return false;
}

bool MapDownloadManager::beginDownloadSelected(float currentLat, float currentLon, bool hasFix) {
  return beginDownload(selectedTarget_, currentLat, currentLon, hasFix);
}

bool MapDownloadManager::beginDownload(uint8_t index, float currentLat, float currentLon, bool hasFix) {
  if (index >= optionCount_ || busy()) {
    return false;
  }

  const MapDownloadTargetInfo& selected = options_[index];
  if (selected.kind == MapDownloadTargetKind::MacroRegion) {
    enterMacro(selected.macroIndex);
    return true;
  }
  if (selected.kind == MapDownloadTargetKind::State) {
    if (kBrazilStates[selected.stateIndex].packageCount == 0) {
      setStatus("MAPAS EM BREVE");
      return false;
    }
    enterState(selected.stateIndex);
    return true;
  }
  if (selected.kind == MapDownloadTargetKind::StateBundle) {
    return beginStateDownload(selected.stateIndex);
  }
  if (selected.kind == MapDownloadTargetKind::PageNext || selected.kind == MapDownloadTargetKind::PagePrev) {
    selectTarget(index);
    return true;
  }
  if (selected.kind == MapDownloadTargetKind::DeleteMaps || selected.kind == MapDownloadTargetKind::DownloadedMaps) {
    return executeLocalAction(index, currentLat, currentLon, hasFix);
  }
  if (selected.kind == MapDownloadTargetKind::CurrentRegion) {
    if (!hasFix || !validCoordinate(currentLat, currentLon)) {
      fail("SEM GPS PARA REGIAO ATUAL");
      return false;
    }
    const uint16_t packageIndex = packageForCoordinate(currentLat, currentLon);
    if (packageIndex == kNoPackageIndex) {
      fail("SEM PACOTE PARA GPS");
      return false;
    }
    if (view_ == MapDownloadView::Packages && kBrazilMapPackages[packageIndex].stateIndex != selectedStateIndex_) {
      fail("GPS FORA DO ESTADO");
      return false;
    }

    char id[20];
    char name[28];
    char file[32];
    packageId(packageIndex, id, sizeof(id));
    packageName(packageIndex, name, sizeof(name));
    snprintf(file, sizeof(file), "%s.brmap", id);
    MapDownloadTargetInfo gpsTarget = {id,
                                       "REGIAO ATUAL",
                                       name,
                                       file,
                                       currentLat,
                                       currentLon,
                                       100,
                                       1,
                                       MapDownloadTargetKind::CurrentRegion,
                                       kBrazilStates[kBrazilMapPackages[packageIndex].stateIndex].macroIndex,
                                       kBrazilMapPackages[packageIndex].stateIndex,
                                       packageIndex};
    return beginPackageDownload(gpsTarget);
  }
  if (selected.kind == MapDownloadTargetKind::Package) {
    return beginPackageDownload(selected);
  }

  return false;
}

uint8_t MapDownloadManager::regionCount() const {
  return 0;
}

const MapRegionInfo& MapDownloadManager::region(uint8_t index) const {
  (void)index;
  return kNoLegacyRegion;
}

void MapDownloadManager::selectRegion(uint8_t index) {
  (void)index;
}

bool MapDownloadManager::beginDownloadSelected() {
  return beginDownloadSelected(0.0F, 0.0F, false);
}

bool MapDownloadManager::beginDownload(uint8_t index) {
  return beginDownload(index, 0.0F, 0.0F, false);
}

void MapDownloadManager::cancel() {
  if (!busy()) {
    return;
  }
  closeTransfer();
  fs::FS* fs = storage_ ? storage_->filesystem() : nullptr;
  if (fs && tempPath_[0] != '\0' && fs->exists(tempPath_)) {
    fs->remove(tempPath_);
  }
  state_ = MapDownloadState::Canceled;
  bulkActive_ = false;
  progressPercent_ = 0;
  setStatus("DOWNLOAD CANCELADO");
}

void MapDownloadManager::resetStatus() {
  closeTransfer();
  state_ = MapDownloadState::Idle;
  bulkActive_ = false;
  bulkCount_ = 0;
  bulkIndex_ = 0;
  bulkDownloaded_ = 0;
  progressPercent_ = 0;
  downloadedBytes_ = 0;
  contentLength_ = -1;
  waitStartedMs_ = 0;
  lastDataMs_ = 0;
  deleteConfirm_ = false;
  errorText_[0] = '\0';
  setStatus("PRONTO");
}

bool MapDownloadManager::isConfigured() const {
  return MapDownloadConfig::kBaseUrl && MapDownloadConfig::kBaseUrl[0] != '\0';
}

bool MapDownloadManager::regionDownloaded(uint8_t index) const {
  return targetDownloaded(index);
}

void MapDownloadManager::startWhenWifiReady() {
  if (!wifi_) {
    state_ = MapDownloadState::NoWifi;
    fail("WIFI INDISPONIVEL");
    return;
  }

  if (wifi_->isConnected()) {
    startHttpDownload();
    return;
  }

  if (millis() - waitStartedMs_ > MapDownloadConfig::kWifiWaitTimeoutMs) {
    state_ = MapDownloadState::NoWifi;
    fail("WIFI NAO CONECTOU");
  }
}

bool MapDownloadManager::startHttpDownload() {
  if (!storage_ || !storage_->ensureMapDirectories()) {
    state_ = MapDownloadState::NoSd;
    fail("SD NAO PREPARADO");
    return false;
  }

  fs::FS* fs = storage_->filesystem();
  if (!fs) {
    state_ = MapDownloadState::NoSd;
    fail("SD NAO MONTADO");
    return false;
  }

  if (fs->exists(tempPath_)) {
    fs->remove(tempPath_);
  }
  output_ = fs->open(tempPath_, FILE_WRITE);
  if (!output_) {
    state_ = MapDownloadState::NoSd;
    fail("NAO ABRIU ARQUIVO SD");
    return false;
  }

  if (startsWithHttps(url_)) {
    if (MapDownloadConfig::kAllowInsecureTls) {
      secureClient_.setInsecure();
    }
    if (!http_.begin(secureClient_, url_)) {
      fail("FALHA URL HTTPS");
      return false;
    }
  } else {
    if (!http_.begin(plainClient_, url_)) {
      fail("FALHA URL HTTP");
      return false;
    }
  }

  http_.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http_.setTimeout(MapDownloadConfig::kHttpTimeoutMs);
  const int code = http_.GET();
  if (code != HTTP_CODE_OK) {
    char text[48];
    snprintf(text, sizeof(text), "HTTP %d", code);
    fail(text);
    return false;
  }

  contentLength_ = http_.getSize();
  downloadedBytes_ = 0;
  progressPercent_ = 0;
  lastDataMs_ = millis();
  state_ = MapDownloadState::Downloading;
  if (bulkActive_) {
    char text[64];
    snprintf(text,
             sizeof(text),
             "BAIXANDO %u/%u",
             static_cast<unsigned>(bulkIndex_),
             static_cast<unsigned>(bulkCount_));
    setStatus(text);
  } else {
    setStatus("BAIXANDO MAPA");
  }
  Serial.printf("Mapa: baixando %s para %s\n", url_, finalPath_);
  return true;
}

void MapDownloadManager::finishDownload() {
  output_.flush();
  closeTransfer();

  fs::FS* fs = storage_ ? storage_->filesystem() : nullptr;
  if (!fs) {
    fail("SD DESCONECTADO");
    return;
  }
  if (fs->exists(finalPath_)) {
    fs->remove(finalPath_);
  }
  if (!fs->rename(tempPath_, finalPath_)) {
    fail("FALHA AO FINALIZAR ARQUIVO");
    return;
  }

  if (storage_) {
    storage_->refresh();
  }
  const bool manifestOk = createRegionManifest();
  if (storage_) {
    storage_->refresh();
  }
  progressPercent_ = 100;
  state_ = MapDownloadState::Success;
  if (bulkActive_) {
    ++bulkDownloaded_;
    Serial.printf("Mapa: pacote %u/%u concluido\n", static_cast<unsigned>(bulkDownloaded_), static_cast<unsigned>(bulkCount_));
    if (startNextQueuedDownload()) {
      return;
    }
    bulkActive_ = false;
    char text[64];
    snprintf(text, sizeof(text), "ESTADO SALVO %u MAPAS", static_cast<unsigned>(bulkDownloaded_));
    setStatus(text);
  } else {
    setStatus(manifestOk ? "MAPA SALVO NO SD" : "MAPA SALVO SEM MANIFEST");
  }
  Serial.printf("Mapa: download concluido %s (%lu bytes)\n", finalPath_, static_cast<unsigned long>(downloadedBytes_));
}

void MapDownloadManager::fail(const char* message) {
  closeTransfer();
  fs::FS* fs = storage_ ? storage_->filesystem() : nullptr;
  if (fs && tempPath_[0] != '\0' && fs->exists(tempPath_)) {
    fs->remove(tempPath_);
  }
  if (state_ != MapDownloadState::NoWifi && state_ != MapDownloadState::NoSd && state_ != MapDownloadState::NoUrl) {
    state_ = MapDownloadState::Failed;
  }
  bulkActive_ = false;
  snprintf(errorText_, sizeof(errorText_), "%s", message ? message : "");
  setStatus(errorText_);
  Serial.printf("Mapa: %s\n", errorText_);
}

void MapDownloadManager::setStatus(const char* text) {
  snprintf(statusText_, sizeof(statusText_), "%s", text ? text : "");
}

void MapDownloadManager::buildPaths(const MapDownloadTargetInfo& info) {
  snprintf(finalPath_, sizeof(finalPath_), "/maps/regions/%s", info.fileName);
  snprintf(tempPath_, sizeof(tempPath_), "/maps/regions/%s.tmp", info.id);
}

void MapDownloadManager::buildUrl(const MapDownloadTargetInfo& info) {
  const char* base = MapDownloadConfig::kBaseUrl ? MapDownloadConfig::kBaseUrl : "";
  const size_t len = strlen(base);
  if (len > 0 && base[len - 1] == '/') {
    snprintf(url_, sizeof(url_), "%s%s", base, info.fileName);
  } else {
    snprintf(url_, sizeof(url_), "%s/%s", base, info.fileName);
  }
}

void MapDownloadManager::closeTransfer() {
  if (output_) {
    output_.close();
  }
  http_.end();
}

bool MapDownloadManager::createRegionManifest() {
  if (!storage_ || !storage_->ensureMapDirectories()) {
    return false;
  }

  fs::FS* fs = storage_->filesystem();
  if (!fs) {
    return false;
  }

  const uint16_t radius = activeTargetInfo_.radiusKm > 0 ? activeTargetInfo_.radiusKm : 100;
  char dirPath[96];
  char manifestPath[128];
  snprintf(dirPath, sizeof(dirPath), "/maps/regions/%s_%ukm", activeTargetInfo_.id, static_cast<unsigned>(radius));
  if (!fs->exists(dirPath) && !fs->mkdir(dirPath)) {
    return false;
  }
  snprintf(manifestPath, sizeof(manifestPath), "%s/manifest.json", dirPath);

  File manifest = fs->open(manifestPath, FILE_WRITE);
  if (!manifest) {
    return false;
  }

  char line[160];
  manifest.println("{");
  snprintf(line, sizeof(line), "  \"id\": \"%s_%ukm\",", activeTargetInfo_.id, static_cast<unsigned>(radius));
  manifest.println(line);
  snprintf(line, sizeof(line), "  \"name\": \"%s\",", activeTargetInfo_.name);
  manifest.println(line);
  snprintf(line, sizeof(line), "  \"kind\": \"%s\",", targetKindText(activeTargetInfo_.kind));
  manifest.println(line);
  snprintf(line, sizeof(line), "  \"radiusKm\": %u,", static_cast<unsigned>(radius));
  manifest.println(line);
  snprintf(line, sizeof(line), "  \"centerLat\": %.6f,", static_cast<double>(activeCenterLat_));
  manifest.println(line);
  snprintf(line, sizeof(line), "  \"centerLon\": %.6f,", static_cast<double>(activeCenterLon_));
  manifest.println(line);
  snprintf(line, sizeof(line), "  \"packageFile\": \"%s\",", activeTargetInfo_.fileName);
  manifest.println(line);
  snprintf(line, sizeof(line), "  \"format\": \"brmap\",");
  manifest.println(line);
  snprintf(line, sizeof(line), "  \"source\": \"brvario-brazil-catalog\"");
  manifest.println(line);
  manifest.println("}");
  manifest.close();
  return true;
}

bool MapDownloadManager::executeLocalAction(uint8_t index, float currentLat, float currentLon, bool hasFix) {
  (void)currentLat;
  (void)currentLon;
  (void)hasFix;
  if (index >= optionCount_ || busy()) {
    return false;
  }

  selectedTarget_ = index;
  const MapDownloadTargetInfo& info = options_[index];

  if (info.kind == MapDownloadTargetKind::DeleteMaps && !deleteConfirm_) {
    deleteConfirm_ = true;
    state_ = MapDownloadState::Idle;
    progressPercent_ = 0;
    setStatus("APERTE APAGAR DE NOVO");
    return true;
  }

  resetStatus();
  selectedTarget_ = index;

  if (!storage_ || !storage_->refresh()) {
    state_ = MapDownloadState::NoSd;
    fail("SD NAO DISPONIVEL");
    return false;
  }

  if (info.kind == MapDownloadTargetKind::DeleteMaps) {
    if (!storage_->clearMaps()) {
      state_ = MapDownloadState::NoSd;
      fail("FALHA AO LIMPAR MAPAS");
      return false;
    }
    progressPercent_ = 100;
    state_ = MapDownloadState::Success;
    setStatus("MAPAS APAGADOS");
    return true;
  }

  if (info.kind == MapDownloadTargetKind::DownloadedMaps) {
    char text[64];
    const uint32_t kb = static_cast<uint32_t>((storage_->mapsBytes() + 1023ULL) / 1024ULL);
    snprintf(text, sizeof(text), "SD: %u ARQ %lu KB", static_cast<unsigned>(storage_->mapFileCount()), static_cast<unsigned long>(kb));
    state_ = MapDownloadState::Success;
    setStatus(text);
    return true;
  }

  return false;
}

uint16_t MapDownloadManager::packageForCoordinate(float lat, float lon) const {
  if (!validCoordinate(lat, lon)) {
    return kNoPackageIndex;
  }

  const int32_t latE7 = coordToE7(lat);
  const int32_t lonE7 = coordToE7(lon);
  float bestKm = 100000.0F;
  uint16_t bestIndex = kNoPackageIndex;
  for (uint16_t i = 0; i < kBrazilMapPackageCount; ++i) {
    const BrazilMapPackageInfo& package = kBrazilMapPackages[i];
    if (!pointInPackage(package, latE7, lonE7)) {
      continue;
    }
    const float km = distanceKm(lat, lon, packageCenterLat(package), packageCenterLon(package));
    if (km < bestKm) {
      bestKm = km;
      bestIndex = i;
    }
  }
  return bestIndex;
}

bool MapDownloadManager::targetFileExists(const char* fileName) const {
  if (!storage_ || !fileName || fileName[0] == '\0') {
    return false;
  }
  fs::FS* fs = storage_->filesystem();
  if (!fs) {
    return false;
  }

  char path[96];
  snprintf(path, sizeof(path), "/maps/regions/%s", fileName);
  return fs->exists(path);
}

bool MapDownloadManager::targetFileExistsForPackageIndex(uint16_t packageIndex) const {
  if (packageIndex >= kBrazilMapPackageCount) {
    return false;
  }
  char id[20];
  char fileName[32];
  packageId(packageIndex, id, sizeof(id));
  snprintf(fileName, sizeof(fileName), "%s.brmap", id);
  return targetFileExists(fileName);
}

bool MapDownloadManager::manifestExists(const char* id) const {
  if (!storage_ || !id || id[0] == '\0') {
    return false;
  }
  fs::FS* fs = storage_->filesystem();
  if (!fs) {
    return false;
  }

  char path[128];
  snprintf(path, sizeof(path), "/maps/regions/%s_100km/manifest.json", id);
  return fs->exists(path);
}

void MapDownloadManager::refreshOptions() {
  optionCount_ = 0;

  if (view_ == MapDownloadView::MacroRegions) {
    setOption(0, MapDownloadTargetKind::CurrentRegion, "current", "REGIAO ATUAL", "GPS AUTO", "", 0.0F, 0.0F, 100, 0, 0, 0, kNoPackageIndex);
    for (uint8_t i = 0; i < kBrazilMacroRegionCount && optionCount_ < kMaxOptions; ++i) {
      char subtitle[24];
      snprintf(subtitle, sizeof(subtitle), "%u ESTADOS", static_cast<unsigned>(stateCountForMacro(i)));
      setOption(optionCount_,
                MapDownloadTargetKind::MacroRegion,
                kBrazilMacroRegions[i].id,
                kBrazilMacroRegions[i].name,
                subtitle,
                "",
                0.0F,
                0.0F,
                0,
                0,
                i,
                0,
                kNoPackageIndex);
    }
    selectedTarget_ = 0;
    return;
  }

  if (view_ == MapDownloadView::States) {
    const uint8_t stateCount = stateCountForMacro(selectedMacroIndex_);
    const uint8_t start = static_cast<uint8_t>(browserPage_ * kStatePageSize);
    if (browserPage_ > 0) {
      setOption(optionCount_, MapDownloadTargetKind::PagePrev, "prev", "ANTERIOR", "PAGINA", "", 0.0F, 0.0F, 0, 0, selectedMacroIndex_, 0, kNoPackageIndex);
    }

    const uint8_t maxStateSlots = browserPage_ == 0 ? kStatePageSize : static_cast<uint8_t>(kMaxOptions - 1);
    for (uint8_t offset = 0; offset < maxStateSlots && start + offset < stateCount && optionCount_ < kMaxOptions; ++offset) {
      const uint8_t stateIndex = stateIndexForMacroOffset(selectedMacroIndex_, start + offset);
      const BrazilStateInfo& state = kBrazilStates[stateIndex];
      char subtitle[24];
      if (state.packageCount > 0) {
        snprintf(subtitle, sizeof(subtitle), "%u MAPAS", static_cast<unsigned>(state.packageCount));
      } else {
        snprintf(subtitle, sizeof(subtitle), "EM BREVE");
      }
      setOption(optionCount_,
                MapDownloadTargetKind::State,
                state.uf,
                state.uf,
                subtitle,
                "",
                0.0F,
                0.0F,
                0,
                state.packageCount,
                selectedMacroIndex_,
                stateIndex,
                kNoPackageIndex);
    }

    if (start + maxStateSlots < stateCount && optionCount_ < kMaxOptions) {
      setOption(optionCount_, MapDownloadTargetKind::PageNext, "next", "PROXIMA", "PAGINA", "", 0.0F, 0.0F, 0, 0, selectedMacroIndex_, 0, kNoPackageIndex);
    }
    selectedTarget_ = optionCount_ > 0 ? 0 : 0;
    return;
  }

  const BrazilStateInfo& state = kBrazilStates[selectedStateIndex_];
  char subtitle[28];
  snprintf(subtitle, sizeof(subtitle), "%u + BORDA", static_cast<unsigned>(state.packageCount));
  setOption(0,
            MapDownloadTargetKind::StateBundle,
            state.uf,
            "BAIXAR ESTADO",
            subtitle,
            "",
            0.0F,
            0.0F,
            100,
            state.packageCount,
            selectedMacroIndex_,
            selectedStateIndex_,
            kNoPackageIndex);
  setOption(1,
            MapDownloadTargetKind::CurrentRegion,
            "current",
            "REGIAO GPS",
            state.uf,
            "",
            0.0F,
            0.0F,
            100,
            0,
            selectedMacroIndex_,
            selectedStateIndex_,
            kNoPackageIndex);
  setOption(2,
            MapDownloadTargetKind::DownloadedMaps,
            "downloaded",
            "MAPAS NO SD",
            "MEMORIA",
            "",
            0.0F,
            0.0F,
            0,
            0,
            selectedMacroIndex_,
            selectedStateIndex_,
            kNoPackageIndex);
  setOption(3,
            MapDownloadTargetKind::DeleteMaps,
            "clear_maps",
            "EXCLUIR MAPAS",
            "MICROSD",
            "",
            0.0F,
            0.0F,
            0,
            0,
            selectedMacroIndex_,
            selectedStateIndex_,
            kNoPackageIndex);
  selectedTarget_ = 0;
}

void MapDownloadManager::setOption(uint8_t slot,
                                   MapDownloadTargetKind kind,
                                   const char* id,
                                   const char* name,
                                   const char* subtitle,
                                   const char* fileName,
                                   float centerLat,
                                   float centerLon,
                                   uint16_t radiusKm,
                                   uint16_t approxSizeMb,
                                   uint8_t macroIndex,
                                   uint8_t stateIndex,
                                   uint16_t packageIndex) {
  if (slot >= kMaxOptions) {
    return;
  }
  snprintf(optionIds_[slot], sizeof(optionIds_[slot]), "%s", id ? id : "");
  snprintf(optionNames_[slot], sizeof(optionNames_[slot]), "%s", name ? name : "");
  snprintf(optionSubtitles_[slot], sizeof(optionSubtitles_[slot]), "%s", subtitle ? subtitle : "");
  snprintf(optionFiles_[slot], sizeof(optionFiles_[slot]), "%s", fileName ? fileName : "");
  options_[slot] = {optionIds_[slot],
                    optionNames_[slot],
                    optionSubtitles_[slot],
                    optionFiles_[slot],
                    centerLat,
                    centerLon,
                    radiusKm,
                    approxSizeMb,
                    kind,
                    macroIndex,
                    stateIndex,
                    packageIndex};
  if (slot >= optionCount_) {
    optionCount_ = slot + 1;
  }
}

void MapDownloadManager::setPackageOption(uint8_t slot, uint16_t packageIndex) {
  if (packageIndex >= kBrazilMapPackageCount) {
    return;
  }
  const BrazilMapPackageInfo& package = kBrazilMapPackages[packageIndex];
  char id[20];
  char name[28];
  char fileName[32];
  packageId(packageIndex, id, sizeof(id));
  packageName(packageIndex, name, sizeof(name));
  snprintf(fileName, sizeof(fileName), "%s.brmap", id);
  setOption(slot,
            MapDownloadTargetKind::Package,
            id,
            name,
            "100KM",
            fileName,
            packageCenterLat(package),
            packageCenterLon(package),
            100,
            1,
            kBrazilStates[package.stateIndex].macroIndex,
            package.stateIndex,
            packageIndex);
}

uint8_t MapDownloadManager::stateCountForMacro(uint8_t macroIndex) const {
  uint8_t count = 0;
  for (uint8_t i = 0; i < kBrazilStateCount; ++i) {
    if (kBrazilStates[i].macroIndex == macroIndex) {
      ++count;
    }
  }
  return count;
}

uint8_t MapDownloadManager::stateIndexForMacroOffset(uint8_t macroIndex, uint8_t offset) const {
  uint8_t seen = 0;
  for (uint8_t i = 0; i < kBrazilStateCount; ++i) {
    if (kBrazilStates[i].macroIndex != macroIndex) {
      continue;
    }
    if (seen == offset) {
      return i;
    }
    ++seen;
  }
  return 0;
}

void MapDownloadManager::enterMacro(uint8_t macroIndex) {
  selectedMacroIndex_ = macroIndex < kBrazilMacroRegionCount ? macroIndex : 0;
  view_ = MapDownloadView::States;
  browserPage_ = 0;
  refreshOptions();
  setStatus("ESCOLHA ESTADO");
}

void MapDownloadManager::enterState(uint8_t stateIndex) {
  selectedStateIndex_ = stateIndex < kBrazilStateCount ? stateIndex : 0;
  selectedMacroIndex_ = kBrazilStates[selectedStateIndex_].macroIndex;
  view_ = MapDownloadView::Packages;
  browserPage_ = 0;
  refreshOptions();
  setStatus("BAIXE O ESTADO");
}

bool MapDownloadManager::beginPackageDownload(const MapDownloadTargetInfo& info) {
  if (info.packageIndex >= kBrazilMapPackageCount || !info.fileName || info.fileName[0] == '\0') {
    fail("PACOTE INVALIDO");
    return false;
  }

  const bool queued = bulkActive_;
  if (queued) {
    closeTransfer();
    state_ = MapDownloadState::Idle;
    progressPercent_ = 0;
    downloadedBytes_ = 0;
    contentLength_ = -1;
    waitStartedMs_ = 0;
    lastDataMs_ = 0;
    errorText_[0] = '\0';
  } else {
    resetStatus();
  }
  snprintf(activeId_, sizeof(activeId_), "%s", info.id);
  snprintf(activeName_, sizeof(activeName_), "%s", info.name);
  snprintf(activeFile_, sizeof(activeFile_), "%s", info.fileName);
  activeTargetInfo_ = info;
  activeTargetInfo_.id = activeId_;
  activeTargetInfo_.name = activeName_;
  activeTargetInfo_.fileName = activeFile_;
  activePackageIndex_ = info.packageIndex;
  activeCenterLat_ = info.centerLat;
  activeCenterLon_ = info.centerLon;
  activeRadiusKm_ = info.radiusKm;
  selectedTarget_ = selectedTarget_ < optionCount_ ? selectedTarget_ : 0;

  buildPaths(activeTargetInfo_);
  buildUrl(activeTargetInfo_);

  if (!storage_ || !storage_->refresh()) {
    state_ = MapDownloadState::NoSd;
    fail("SD NAO DISPONIVEL");
    return false;
  }
  if (!isConfigured()) {
    state_ = MapDownloadState::NoUrl;
    fail("URL DE MAPAS NAO CONFIGURADA");
    return false;
  }

  if (!wifi_) {
    state_ = MapDownloadState::NoWifi;
    fail("WIFI INDISPONIVEL");
    return false;
  }
  if (!wifi_->isEnabled()) {
    wifi_->setEnabled(true);
  }
  if (!wifi_->isConnected()) {
    wifi_->connectSaved();
    waitStartedMs_ = millis();
    state_ = MapDownloadState::WaitingWifi;
    setStatus(queued ? "WIFI PARA ESTADO" : "AGUARDANDO WIFI");
    return true;
  }

  return startHttpDownload();
}

bool MapDownloadManager::beginStateDownload(uint8_t stateIndex) {
  if (stateIndex >= kBrazilStateCount || kBrazilStates[stateIndex].packageCount == 0 || busy()) {
    setStatus("ESTADO SEM MAPAS");
    return false;
  }

  closeTransfer();
  state_ = MapDownloadState::Idle;
  progressPercent_ = 0;
  downloadedBytes_ = 0;
  contentLength_ = -1;
  waitStartedMs_ = 0;
  lastDataMs_ = 0;
  errorText_[0] = '\0';
  deleteConfirm_ = false;

  if (!storage_ || !storage_->refresh()) {
    state_ = MapDownloadState::NoSd;
    fail("SD NAO DISPONIVEL");
    return false;
  }

  selectedStateIndex_ = stateIndex;
  selectedMacroIndex_ = kBrazilStates[stateIndex].macroIndex;
  buildStateQueue(stateIndex);
  if (bulkCount_ == 0) {
    setStatus("ESTADO JA NO SD");
    return true;
  }

  bulkActive_ = true;
  bulkIndex_ = 0;
  bulkDownloaded_ = 0;
  return startNextQueuedDownload();
}

bool MapDownloadManager::startNextQueuedDownload() {
  while (bulkIndex_ < bulkCount_) {
    const uint16_t packageIndex = bulkQueue_[bulkIndex_++];
    if (packageIndex >= kBrazilMapPackageCount) {
      continue;
    }

    char id[20];
    char name[28];
    char fileName[32];
    packageId(packageIndex, id, sizeof(id));
    packageName(packageIndex, name, sizeof(name));
    snprintf(fileName, sizeof(fileName), "%s.brmap", id);
    if (targetFileExists(fileName)) {
      continue;
    }

    const BrazilMapPackageInfo& package = kBrazilMapPackages[packageIndex];
    MapDownloadTargetInfo queuedTarget = {id,
                                          name,
                                          kBrazilStates[selectedStateIndex_].uf,
                                          fileName,
                                          packageCenterLat(package),
                                          packageCenterLon(package),
                                          100,
                                          1,
                                          MapDownloadTargetKind::StateBundle,
                                          kBrazilStates[package.stateIndex].macroIndex,
                                          package.stateIndex,
                                          packageIndex};
    return beginPackageDownload(queuedTarget);
  }

  return false;
}

void MapDownloadManager::buildStateQueue(uint8_t stateIndex) {
  bulkCount_ = 0;
  bulkIndex_ = 0;
  bulkDownloaded_ = 0;
  if (stateIndex >= kBrazilStateCount) {
    return;
  }

  const BrazilStateInfo& state = kBrazilStates[stateIndex];
  for (uint16_t i = 0; i < state.packageCount && bulkCount_ < kMaxBulkQueue; ++i) {
    const uint16_t packageIndex = state.firstPackage + i;
    if (packageIndex < kBrazilMapPackageCount && !targetFileExistsForPackageIndex(packageIndex)) {
      bulkQueue_[bulkCount_++] = packageIndex;
    }
  }

  // Include bordering packages from neighbouring states so flights crossing a
  // state line still have terrain immediately beyond the boundary.
  for (uint16_t packageIndex = 0; packageIndex < kBrazilMapPackageCount && bulkCount_ < kMaxBulkQueue; ++packageIndex) {
    if (kBrazilMapPackages[packageIndex].stateIndex == stateIndex || !packageTouchesStatePackage(packageIndex, stateIndex)) {
      continue;
    }
    if (!queueContains(packageIndex) && !targetFileExistsForPackageIndex(packageIndex)) {
      bulkQueue_[bulkCount_++] = packageIndex;
    }
  }
}

bool MapDownloadManager::queueContains(uint16_t packageIndex) const {
  for (uint16_t i = 0; i < bulkCount_; ++i) {
    if (bulkQueue_[i] == packageIndex) {
      return true;
    }
  }
  return false;
}

bool MapDownloadManager::packageTouchesStatePackage(uint16_t packageIndex, uint8_t stateIndex) const {
  if (packageIndex >= kBrazilMapPackageCount || stateIndex >= kBrazilStateCount) {
    return false;
  }
  const BrazilMapPackageInfo& candidate = kBrazilMapPackages[packageIndex];
  const BrazilStateInfo& state = kBrazilStates[stateIndex];
  for (uint16_t i = 0; i < state.packageCount; ++i) {
    const uint16_t statePackageIndex = state.firstPackage + i;
    if (statePackageIndex < kBrazilMapPackageCount && packageIntersects(candidate, kBrazilMapPackages[statePackageIndex])) {
      return true;
    }
  }
  return false;
}
