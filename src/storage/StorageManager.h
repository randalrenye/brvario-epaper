#pragma once

#include <Arduino.h>
#include <FS.h>
#include <stdint.h>

class StorageManager {
 public:
  bool begin();
  bool refresh();
  bool ensureMounted();
  void end();

  bool mounted() const { return mounted_; }
  const char* statusText() const { return statusText_; }
  const char* cardTypeText() const { return cardTypeText_; }
  uint64_t cardSizeBytes() const { return cardSizeBytes_; }
  uint64_t totalBytes() const { return totalBytes_; }
  uint64_t usedBytes() const { return usedBytes_; }
  uint64_t freeBytes() const { return totalBytes_ > usedBytes_ ? totalBytes_ - usedBytes_ : 0; }
  uint64_t mapsBytes() const { return mapsBytes_; }
  uint16_t mapFileCount() const { return mapFileCount_; }
  bool mapsReady() const { return mapsReady_; }
  fs::FS* filesystem();

  bool ensureMapDirectories();
  bool clearMaps();
  bool hasRegionPackage(const char* regionName) const;

 private:
  bool mounted_ = false;
  bool mapsReady_ = false;
  char statusText_[64] = "SD NAO INICIADO";
  char cardTypeText_[12] = "---";
  uint64_t cardSizeBytes_ = 0;
  uint64_t totalBytes_ = 0;
  uint64_t usedBytes_ = 0;
  uint64_t mapsBytes_ = 0;
  uint16_t mapFileCount_ = 0;

  bool mountCard();
  void updateStats();
  void setStatus(const char* text);
  bool ensureDir(const char* path);
  bool removeRecursive(const char* path);
  void scanDirectory(const char* path, uint64_t& bytes, uint16_t& files) const;
};
