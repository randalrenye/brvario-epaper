#include "map/OfflineMapPackage.h"

#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/AppConfig.h"
#include "display/EpdDisplay.h"

namespace {

static constexpr char kRegionsRoot[] = "/maps/regions";
static constexpr uint32_t kMagic = 0x50414D42UL;  // "BMAP" little-endian.
static constexpr uint16_t kFormatVersion = 1;
static constexpr uint16_t kFormatVersionRelief = 2;
static constexpr uint16_t kMaxPolylinePoints = 96;
static constexpr uint16_t kMaxRasterWidth = 1440;
static constexpr uint32_t kMaxIndexedLines = 30000;
static constexpr float kDegToRad = 0.01745329251994329577F;
static constexpr float kMetersPerLatDeg = 111320.0F;
static char cachedMapPath[96] = {};

#pragma pack(push, 1)
struct BrmapHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t headerSize;
  int32_t latMinE7;
  int32_t latMaxE7;
  int32_t lonMinE7;
  int32_t lonMaxE7;
  uint32_t lineCount;
  uint32_t waypointCount;
  char name[32];
  uint8_t reserved[16];
};

struct BrmapLineRecord {
  uint8_t type;
  uint8_t flags;
  uint16_t pointCount;
  int32_t latMinE7;
  int32_t latMaxE7;
  int32_t lonMinE7;
  int32_t lonMaxE7;
};

struct BrmapPointRecord {
  int32_t latE7;
  int32_t lonE7;
};

struct BrmapRasterInfo {
  uint16_t width;
  uint16_t height;
  int32_t latMinE7;
  int32_t latMaxE7;
  int32_t lonMinE7;
  int32_t lonMaxE7;
  uint32_t dataSize;
  uint8_t format;
  uint8_t reserved[7];
};

struct BrmapWaypointRecord {
  uint8_t type;
  int32_t latE7;
  int32_t lonE7;
  char name[24];
};
#pragma pack(pop)

struct ScreenPoint {
  int32_t x;
  int32_t y;
};

struct CachedLineRecord {
  uint32_t pointOffset;
  uint16_t pointCount;
  uint8_t type;
  uint8_t flags;
  int32_t latMinE7;
  int32_t latMaxE7;
  int32_t lonMinE7;
  int32_t lonMaxE7;
};

static CachedLineRecord* cachedLineRecords = nullptr;
static uint32_t cachedLineRecordCount = 0;
static uint32_t cachedLineFileSize = 0;
static uint32_t cachedWaypointOffset = 0;
static char cachedLinePath[96] = {};

int32_t latToE7(float value) {
  return static_cast<int32_t>(value * 10000000.0F + (value >= 0.0F ? 0.5F : -0.5F));
}

float e7ToDeg(int32_t value) {
  return static_cast<float>(value) / 10000000.0F;
}

bool readExact(File& file, void* dst, size_t size) {
  return file.read(static_cast<uint8_t*>(dst), size) == size;
}

void clearLineIndexCache() {
  if (cachedLineRecords) {
    free(cachedLineRecords);
  }
  cachedLineRecords = nullptr;
  cachedLineRecordCount = 0;
  cachedLineFileSize = 0;
  cachedWaypointOffset = 0;
  cachedLinePath[0] = '\0';
}

bool hasMapExtension(const char* name) {
  if (!name) return false;
  const char* dot = strrchr(name, '.');
  return dot && (strcasecmp(dot, ".brmap") == 0 || strcasecmp(dot, ".brvario") == 0 || strcasecmp(dot, ".bin") == 0);
}

const char* baseName(const char* path) {
  if (!path) return "";
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

bool headerCovers(const BrmapHeader& header, float lat, float lon) {
  const int32_t latE7 = latToE7(lat);
  const int32_t lonE7 = latToE7(lon);
  return latE7 >= header.latMinE7 && latE7 <= header.latMaxE7 && lonE7 >= header.lonMinE7 && lonE7 <= header.lonMaxE7;
}

void writePixel4(uint8_t* fb, int32_t x, int32_t y, uint8_t value) {
  if (!fb || x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) return;
  value &= 0x0F;
  uint8_t& packed = fb[static_cast<size_t>(y) * EPD_WIDTH / 2 + x / 2];
  if (x & 1) {
    packed = (packed & 0x0F) | (value << 4);
  } else {
    packed = (packed & 0xF0) | value;
  }
}

uint8_t packedRasterPixel(const uint8_t* row, int32_t x) {
  const uint8_t packed = row[x / 2];
  return (x & 1) ? ((packed >> 4) & 0x0F) : (packed & 0x0F);
}

uint8_t normalRasterToneForEpaper(uint8_t value) {
  if (value >= 14) {
    return value;
  }
  if (value >= 12) {
    return static_cast<uint8_t>(value + 1);
  }
  if (value <= 7) {
    return static_cast<uint8_t>(value + 3);
  }
  return static_cast<uint8_t>(value + 2);
}

uint8_t lightRasterToneForEpaper(uint8_t value) {
  const int32_t lifted = static_cast<int32_t>(value) + 5;
  return static_cast<uint8_t>(lifted > 15 ? 15 : lifted);
}

uint8_t mapRasterToneForEpaper(uint8_t value, float metersPerPixel) {
  // Keep the hypsometric tint useful up close, but avoid visual clutter when the
  // pilot zooms out to see a wider area.
  if (metersPerPixel >= 20.0F) {
    return 15;
  }
  if (metersPerPixel >= 14.0F) {
    return lightRasterToneForEpaper(value);
  }
  return normalRasterToneForEpaper(value);
}

bool loadRasterRow(File& file, uint32_t dataOffset, uint32_t stride, uint8_t* row, int32_t& loadedRow, int32_t rowIndex) {
  if (rowIndex == loadedRow) {
    return true;
  }
  if (!file.seek(dataOffset + static_cast<uint32_t>(rowIndex) * stride)) {
    return false;
  }
  if (file.read(row, stride) != static_cast<int>(stride)) {
    return false;
  }
  loadedRow = rowIndex;
  return true;
}

uint8_t interpolateRaster4(const uint8_t* topRow, const uint8_t* bottomRow, int32_t x0, int32_t x1, float fx, float fy) {
  const float v00 = static_cast<float>(packedRasterPixel(topRow, x0));
  const float v10 = static_cast<float>(packedRasterPixel(topRow, x1));
  const float v01 = static_cast<float>(packedRasterPixel(bottomRow, x0));
  const float v11 = static_cast<float>(packedRasterPixel(bottomRow, x1));
  const float top = v00 + (v10 - v00) * fx;
  const float bottom = v01 + (v11 - v01) * fx;
  const int32_t value = static_cast<int32_t>(top + (bottom - top) * fy + 0.5F);
  return static_cast<uint8_t>(value < 0 ? 0 : (value > 15 ? 15 : value));
}

void viewBoundsE7(const OfflineMapPackage::View& view, int32_t& latMin, int32_t& latMax, int32_t& lonMin, int32_t& lonMax) {
  const float halfHeightM = static_cast<float>(view.bounds.height) * view.metersPerPixel * 0.5F;
  const float halfWidthM = static_cast<float>(view.bounds.width) * view.metersPerPixel * 0.5F;
  const float cosLat = fmaxf(0.15F, cosf(view.centerLat * kDegToRad));
  const float latDelta = halfHeightM / kMetersPerLatDeg;
  const float lonDelta = halfWidthM / (kMetersPerLatDeg * cosLat);
  latMin = latToE7(view.centerLat - latDelta);
  latMax = latToE7(view.centerLat + latDelta);
  lonMin = latToE7(view.centerLon - lonDelta);
  lonMax = latToE7(view.centerLon + lonDelta);
}

bool bboxIntersects(int32_t aMin, int32_t aMax, int32_t bMin, int32_t bMax) {
  return aMin <= bMax && aMax >= bMin;
}

ScreenPoint projectPoint(const OfflineMapPackage::View& view, int32_t latE7, int32_t lonE7) {
  const float lat = e7ToDeg(latE7);
  const float lon = e7ToDeg(lonE7);
  const float cosLat = fmaxf(0.15F, cosf(view.centerLat * kDegToRad));
  const float eastM = (lon - view.centerLon) * kMetersPerLatDeg * cosLat;
  const float northM = (lat - view.centerLat) * kMetersPerLatDeg;
  return {
      view.pilotX + static_cast<int32_t>(eastM / view.metersPerPixel + (eastM >= 0.0F ? 0.5F : -0.5F)),
      view.pilotY - static_cast<int32_t>(northM / view.metersPerPixel + (northM >= 0.0F ? 0.5F : -0.5F)),
  };
}

void distanceAndBearing(float fromLat, float fromLon, float toLat, float toLon, float& distanceKm, float& bearingDeg) {
  const float lat1 = fromLat * kDegToRad;
  const float lat2 = toLat * kDegToRad;
  const float dLat = (toLat - fromLat) * kDegToRad;
  const float dLon = (toLon - fromLon) * kDegToRad;
  const float sinHalfLat = sinf(dLat * 0.5F);
  const float sinHalfLon = sinf(dLon * 0.5F);
  const float a = sinHalfLat * sinHalfLat + cosf(lat1) * cosf(lat2) * sinHalfLon * sinHalfLon;
  const float c = 2.0F * atan2f(sqrtf(a), sqrtf(fmaxf(0.0F, 1.0F - a)));
  distanceKm = 6371.0F * c;

  const float y = sinf(dLon) * cosf(lat2);
  const float x = cosf(lat1) * sinf(lat2) - sinf(lat1) * cosf(lat2) * cosf(dLon);
  bearingDeg = atan2f(y, x) / kDegToRad;
  while (bearingDeg < 0.0F) bearingDeg += 360.0F;
  while (bearingDeg >= 360.0F) bearingDeg -= 360.0F;
}

uint8_t featureColor(uint8_t type) {
  switch (static_cast<OfflineMapFeatureType>(type)) {
    case OfflineMapFeatureType::Road:
    case OfflineMapFeatureType::Ramp:
      return AppConfig::kBlack;
    case OfflineMapFeatureType::Trail:
    case OfflineMapFeatureType::Contour:
      return 0x66;
    case OfflineMapFeatureType::IndexContour:
      return 0x33;
    case OfflineMapFeatureType::River:
      return AppConfig::kLight;
    default:
      return 0x66;
  }
}

uint8_t featureThickness(uint8_t type) {
  if (type == static_cast<uint8_t>(OfflineMapFeatureType::IndexContour)) {
    return 3;
  }
  return type == static_cast<uint8_t>(OfflineMapFeatureType::Road) || type == static_cast<uint8_t>(OfflineMapFeatureType::River) ||
                 type == static_cast<uint8_t>(OfflineMapFeatureType::Ramp)
             ? 2
             : 1;
}

int clipCode(const Rect_t& r, int32_t x, int32_t y) {
  int code = 0;
  const int32_t xMax = r.x + r.width - 1;
  const int32_t yMax = r.y + r.height - 1;
  if (x < r.x) code |= 1;
  if (x > xMax) code |= 2;
  if (y < r.y) code |= 4;
  if (y > yMax) code |= 8;
  return code;
}

bool clipLine(const Rect_t& r, ScreenPoint& a, ScreenPoint& b) {
  int codeA = clipCode(r, a.x, a.y);
  int codeB = clipCode(r, b.x, b.y);
  const int32_t xMax = r.x + r.width - 1;
  const int32_t yMax = r.y + r.height - 1;

  while (true) {
    if ((codeA | codeB) == 0) return true;
    if (codeA & codeB) return false;

    const int out = codeA ? codeA : codeB;
    int32_t x = 0;
    int32_t y = 0;
    if (out & 8) {
      x = a.x + (b.x - a.x) * (yMax - a.y) / (b.y - a.y);
      y = yMax;
    } else if (out & 4) {
      x = a.x + (b.x - a.x) * (r.y - a.y) / (b.y - a.y);
      y = r.y;
    } else if (out & 2) {
      y = a.y + (b.y - a.y) * (xMax - a.x) / (b.x - a.x);
      x = xMax;
    } else {
      y = a.y + (b.y - a.y) * (r.x - a.x) / (b.x - a.x);
      x = r.x;
    }

    if (out == codeA) {
      a = {x, y};
      codeA = clipCode(r, a.x, a.y);
    } else {
      b = {x, y};
      codeB = clipCode(r, b.x, b.y);
    }
  }
}

void drawLine(EpdDisplay& display, ScreenPoint a, ScreenPoint b, uint8_t color, uint8_t thickness) {
  if (thickness >= 3) {
    display.drawLine(a.x, a.y, b.x, b.y, color);
    display.drawLine(a.x + 1, a.y, b.x + 1, b.y, color);
    display.drawLine(a.x, a.y + 1, b.x, b.y + 1, color);
  } else if (thickness >= 2) {
    display.drawLine(a.x, a.y, b.x, b.y, color);
    display.drawLine(a.x + 1, a.y, b.x + 1, b.y, color);
    display.drawLine(a.x, a.y + 1, b.x, b.y + 1, color);
  } else {
    display.drawLine(a.x, a.y, b.x, b.y, color);
  }
}

bool ensureLineIndex(File& file, const char* path, const BrmapHeader& header, uint32_t lineOffset) {
  const uint32_t fileSize = file.size();
  if (cachedLineRecords && cachedLineRecordCount == header.lineCount && cachedLineFileSize == fileSize &&
      strncmp(cachedLinePath, path, sizeof(cachedLinePath)) == 0) {
    return true;
  }

  clearLineIndexCache();
  if (header.lineCount == 0 || header.lineCount > kMaxIndexedLines) {
    return false;
  }

  const size_t bytes = static_cast<size_t>(header.lineCount) * sizeof(CachedLineRecord);
  cachedLineRecords = static_cast<CachedLineRecord*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!cachedLineRecords) {
    cachedLineRecords = static_cast<CachedLineRecord*>(malloc(bytes));
  }
  if (!cachedLineRecords) {
    return false;
  }

  if (!file.seek(lineOffset)) {
    clearLineIndexCache();
    return false;
  }

  for (uint32_t lineIndex = 0; lineIndex < header.lineCount; ++lineIndex) {
    BrmapLineRecord line = {};
    if (!readExact(file, &line, sizeof(line))) {
      clearLineIndexCache();
      return false;
    }

    const uint32_t pointOffset = file.position();
    cachedLineRecords[lineIndex] = {
        pointOffset,
        line.pointCount,
        line.type,
        line.flags,
        line.latMinE7,
        line.latMaxE7,
        line.lonMinE7,
        line.lonMaxE7,
    };

    const uint32_t nextOffset = pointOffset + static_cast<uint32_t>(line.pointCount) * sizeof(BrmapPointRecord);
    if (!file.seek(nextOffset)) {
      clearLineIndexCache();
      return false;
    }
  }

  cachedLineRecordCount = header.lineCount;
  cachedLineFileSize = fileSize;
  cachedWaypointOffset = file.position();
  snprintf(cachedLinePath, sizeof(cachedLinePath), "%s", path ? path : "");
  return true;
}

bool renderRaster(File& file, const BrmapRasterInfo& raster, uint32_t dataOffset, EpdDisplay& display, const OfflineMapPackage::View& view) {
  if (raster.format != 1 || raster.width == 0 || raster.height == 0 || raster.width > kMaxRasterWidth) {
    return false;
  }

  const uint32_t stride = (static_cast<uint32_t>(raster.width) + 1UL) / 2UL;
  if (raster.dataSize < stride * static_cast<uint32_t>(raster.height)) {
    return false;
  }

  uint8_t* fb = display.framebuffer();
  if (!fb) return false;

  uint8_t row0[(kMaxRasterWidth + 1) / 2] = {};
  uint8_t row1[(kMaxRasterWidth + 1) / 2] = {};
  int32_t loadedRow0 = -1;
  int32_t loadedRow1 = -1;
  const float rasterLatMin = e7ToDeg(raster.latMinE7);
  const float rasterLatMax = e7ToDeg(raster.latMaxE7);
  const float rasterLonMin = e7ToDeg(raster.lonMinE7);
  const float rasterLonMax = e7ToDeg(raster.lonMaxE7);
  const float latSpan = rasterLatMax - rasterLatMin;
  const float lonSpan = rasterLonMax - rasterLonMin;
  if (latSpan <= 0.0F || lonSpan <= 0.0F) {
    return false;
  }

  const float cosLat = fmaxf(0.15F, cosf(view.centerLat * kDegToRad));
  for (int32_t y = view.bounds.y; y < view.bounds.y + view.bounds.height; ++y) {
    const float northM = static_cast<float>(view.pilotY - y) * view.metersPerPixel;
    const float lat = view.centerLat + northM / kMetersPerLatDeg;
    if (lat < rasterLatMin || lat > rasterLatMax) continue;
    const float rasterYf = (rasterLatMax - lat) * static_cast<float>(raster.height - 1) / latSpan;
    int32_t rasterY0 = static_cast<int32_t>(floorf(rasterYf));
    if (rasterY0 < 0 || rasterY0 >= raster.height) continue;
    int32_t rasterY1 = rasterY0 + 1;
    if (rasterY1 >= raster.height) rasterY1 = rasterY0;
    const float fy = rasterYf - static_cast<float>(rasterY0);

    if (!loadRasterRow(file, dataOffset, stride, row0, loadedRow0, rasterY0)) {
      return false;
    }
    const uint8_t* topRow = row0;
    const uint8_t* bottomRow = row0;
    if (rasterY1 != rasterY0) {
      if (!loadRasterRow(file, dataOffset, stride, row1, loadedRow1, rasterY1)) {
        return false;
      }
      bottomRow = row1;
    }

    for (int32_t x = view.bounds.x; x < view.bounds.x + view.bounds.width; ++x) {
      const float eastM = static_cast<float>(x - view.pilotX) * view.metersPerPixel;
      const float lon = view.centerLon + eastM / (kMetersPerLatDeg * cosLat);
      if (lon < rasterLonMin || lon > rasterLonMax) continue;
      const float rasterXf = (lon - rasterLonMin) * static_cast<float>(raster.width - 1) / lonSpan;
      int32_t rasterX0 = static_cast<int32_t>(floorf(rasterXf));
      if (rasterX0 < 0 || rasterX0 >= raster.width) continue;
      int32_t rasterX1 = rasterX0 + 1;
      if (rasterX1 >= raster.width) rasterX1 = rasterX0;
      const float fx = rasterXf - static_cast<float>(rasterX0);
      const uint8_t value = mapRasterToneForEpaper(interpolateRaster4(topRow, bottomRow, rasterX0, rasterX1, fx, fy),
                                                   view.metersPerPixel);
      writePixel4(fb, x, y, value);
    }
  }

  return true;
}

void copyPackageName(char* dst, size_t dstSize, const char* source, const BrmapHeader& header) {
  const char* name = header.name[0] ? header.name : baseName(source);
  snprintf(dst, dstSize, "%s", name);
}

}  // namespace

bool OfflineMapPackage::renderCovering(fs::FS& fs, EpdDisplay& display, const View& view, OfflineMapRenderStats& stats) {
  stats = {};

  if (cachedMapPath[0] != '\0' && renderFile(fs, cachedMapPath, display, view, stats)) {
    return true;
  }

  File root = fs.open(kRegionsRoot);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return false;
  }

  File entry = root.openNextFile();
  while (entry) {
    const bool isDir = entry.isDirectory();
    const char* entryName = entry.name();
    char path[96];
    if (entryName && entryName[0] == '/') {
      snprintf(path, sizeof(path), "%s", entryName);
    } else {
      snprintf(path, sizeof(path), "%s/%s", kRegionsRoot, entryName ? entryName : "");
    }
    entry.close();

    if (!isDir && hasMapExtension(path) && renderFile(fs, path, display, view, stats)) {
      snprintf(cachedMapPath, sizeof(cachedMapPath), "%s", path);
      root.close();
      return true;
    }

    entry = root.openNextFile();
  }

  root.close();
  return false;
}

void OfflineMapPackage::invalidateCache() {
  cachedMapPath[0] = '\0';
  clearLineIndexCache();
}

bool OfflineMapPackage::renderFile(fs::FS& fs, const char* path, EpdDisplay& display, const View& view, OfflineMapRenderStats& stats) {
  File file = fs.open(path, FILE_READ);
  if (!file) return false;

  BrmapHeader header = {};
  if (!readExact(file, &header, sizeof(header)) || header.magic != kMagic ||
      (header.version != kFormatVersion && header.version != kFormatVersionRelief) ||
      header.headerSize < sizeof(BrmapHeader) || !headerCovers(header, view.centerLat, view.centerLon)) {
    file.close();
    return false;
  }

  uint32_t lineOffset = header.headerSize;
  BrmapRasterInfo raster = {};
  if (header.version == kFormatVersionRelief) {
    if (header.headerSize < sizeof(BrmapHeader) + sizeof(BrmapRasterInfo) || !readExact(file, &raster, sizeof(raster))) {
      file.close();
      return false;
    }
    renderRaster(file, raster, header.headerSize, display, view);
    lineOffset = header.headerSize + raster.dataSize;
  }

  int32_t viewLatMin = 0;
  int32_t viewLatMax = 0;
  int32_t viewLonMin = 0;
  int32_t viewLonMax = 0;
  viewBoundsE7(view, viewLatMin, viewLatMax, viewLonMin, viewLonMax);

  stats.foundPackage = true;
  copyPackageName(stats.packageName, sizeof(stats.packageName), path, header);

  BrmapPointRecord points[kMaxPolylinePoints];
  const bool indexed = ensureLineIndex(file, path, header, lineOffset);
  if (indexed) {
    for (uint32_t lineIndex = 0; lineIndex < cachedLineRecordCount; ++lineIndex) {
      const CachedLineRecord& line = cachedLineRecords[lineIndex];
      const uint16_t count = line.pointCount > kMaxPolylinePoints ? kMaxPolylinePoints : line.pointCount;
      const bool visible = bboxIntersects(line.latMinE7, line.latMaxE7, viewLatMin, viewLatMax) &&
                           bboxIntersects(line.lonMinE7, line.lonMaxE7, viewLonMin, viewLonMax);

      if (!visible || count < 2) {
        continue;
      }
      if (!file.seek(line.pointOffset)) {
        continue;
      }
      bool readOk = true;
      for (uint16_t i = 0; i < count; ++i) {
        if (!readExact(file, &points[i], sizeof(points[i]))) {
          readOk = false;
          break;
        }
      }
      if (!readOk) {
        continue;
      }

      const uint8_t color = featureColor(line.type);
      const uint8_t thickness = featureThickness(line.type);
      for (uint16_t i = 1; i < count; ++i) {
        ScreenPoint a = projectPoint(view, points[i - 1].latE7, points[i - 1].lonE7);
        ScreenPoint b = projectPoint(view, points[i].latE7, points[i].lonE7);
        if (clipLine(view.bounds, a, b)) {
          drawLine(display, a, b, color, thickness);
          ++stats.linesDrawn;
          stats.pointsDrawn += 2;
        }
      }
    }
    file.seek(cachedWaypointOffset);
  } else {
    if (!file.seek(lineOffset)) {
      file.close();
      return false;
    }

    for (uint32_t lineIndex = 0; lineIndex < header.lineCount; ++lineIndex) {
      BrmapLineRecord line = {};
      if (!readExact(file, &line, sizeof(line))) break;
      const uint16_t count = line.pointCount > kMaxPolylinePoints ? kMaxPolylinePoints : line.pointCount;
      const bool visible = bboxIntersects(line.latMinE7, line.latMaxE7, viewLatMin, viewLatMax) &&
                           bboxIntersects(line.lonMinE7, line.lonMaxE7, viewLonMin, viewLonMax);

      for (uint16_t i = 0; i < line.pointCount; ++i) {
        BrmapPointRecord point = {};
        if (!readExact(file, &point, sizeof(point))) {
          file.close();
          return stats.foundPackage;
        }
        if (i < count) {
          points[i] = point;
        }
      }

      if (!visible || count < 2) {
        continue;
      }

      const uint8_t color = featureColor(line.type);
      const uint8_t thickness = featureThickness(line.type);
      for (uint16_t i = 1; i < count; ++i) {
        ScreenPoint a = projectPoint(view, points[i - 1].latE7, points[i - 1].lonE7);
        ScreenPoint b = projectPoint(view, points[i].latE7, points[i].lonE7);
        if (clipLine(view.bounds, a, b)) {
          drawLine(display, a, b, color, thickness);
          ++stats.linesDrawn;
          stats.pointsDrawn += 2;
        }
      }
    }
  }

  for (uint32_t i = 0; i < header.waypointCount; ++i) {
    BrmapWaypointRecord waypoint = {};
    if (!readExact(file, &waypoint, sizeof(waypoint))) break;

    float distanceKm = 0.0F;
    float bearingDeg = 0.0F;
    distanceAndBearing(view.centerLat, view.centerLon, e7ToDeg(waypoint.latE7), e7ToDeg(waypoint.lonE7), distanceKm, bearingDeg);
    if (waypoint.name[0] != '\0' && (!stats.hasNearestWaypoint || distanceKm < stats.nearestWaypointDistanceKm)) {
      stats.hasNearestWaypoint = true;
      stats.nearestWaypointDistanceKm = distanceKm;
      stats.nearestWaypointBearingDeg = bearingDeg;
      snprintf(stats.nearestWaypointName, sizeof(stats.nearestWaypointName), "%s", waypoint.name);
    }

    if (waypoint.latE7 < viewLatMin || waypoint.latE7 > viewLatMax || waypoint.lonE7 < viewLonMin || waypoint.lonE7 > viewLonMax) {
      continue;
    }
    const ScreenPoint p = projectPoint(view, waypoint.latE7, waypoint.lonE7);
    if (clipCode(view.bounds, p.x, p.y) != 0) {
      continue;
    }
    display.fillCircle(p.x, p.y, waypoint.type == static_cast<uint8_t>(OfflineMapFeatureType::Ramp) ? 4 : 3, AppConfig::kBlack);
    if (waypoint.name[0] != '\0' && p.x < view.bounds.x + view.bounds.width - 80 && p.y < view.bounds.y + view.bounds.height - 14) {
      display.drawSmallText(waypoint.name, p.x + 7, p.y - 4, 1, AppConfig::kBlack);
      ++stats.labelsDrawn;
    }
  }

  file.close();
  return true;
}
