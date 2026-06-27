#pragma once

#include <stdint.h>

enum class WindQuality : uint8_t {
  None = 0,
  Low,
  Medium,
  High,
};

class WindEstimator {
 public:
  static constexpr uint8_t kSectorCount = 16;

  bool update(float trackDeg, float groundSpeedKmh, float latitudeDeg, float longitudeDeg, uint32_t timestampMs);

  bool hasWind() const { return hasWind_; }
  float getWindSpeed() const { return windSpeedKmh_; }
  float getWindDirectionFrom() const { return windDirectionFromDeg_; }
  float getWindDirectionTo() const { return windDirectionToDeg_; }
  WindQuality getQuality() const { return quality_; }

 private:
  struct SectorSample {
    float speedKmh = 0.0F;
    float eastKmh = 0.0F;
    float northKmh = 0.0F;
    uint32_t updatedMs = 0;
    bool valid = false;
  };

  SectorSample sectors_[kSectorCount] = {};
  bool hasWind_ = false;
  float windSpeedKmh_ = 0.0F;
  float windDirectionFromDeg_ = 0.0F;
  float windDirectionToDeg_ = 180.0F;
  WindQuality quality_ = WindQuality::None;

  uint8_t sectorForTrack(float trackDeg) const;
  float sectorCenterDeg(uint8_t sector) const;
  bool isRecent(const SectorSample& sample, uint32_t nowMs) const;
  bool sectorsAreOpposite(uint8_t a, uint8_t b) const;
  uint8_t largestRecentGap(uint32_t nowMs) const;
  void compute(uint32_t nowMs);
};
