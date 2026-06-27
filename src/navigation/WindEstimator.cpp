#include "navigation/WindEstimator.h"

#include <math.h>

namespace {

static constexpr float kSectorWidthDeg = 360.0F / WindEstimator::kSectorCount;
static constexpr float kMinGroundSpeedKmh = 5.0F;
static constexpr uint32_t kSectorTimeoutMs = 240000UL;
static constexpr uint32_t kHighQualityWindowMs = 90000UL;
static constexpr float kSectorSpeedAlpha = 0.28F;
static constexpr float kOutputAlpha = 0.18F;
static constexpr float kMinUsefulWindKmh = 1.0F;

float normalizeDeg(float value) {
  while (value < 0.0F) value += 360.0F;
  while (value >= 360.0F) value -= 360.0F;
  return value;
}

float shortestAngleDelta(float fromDeg, float toDeg) {
  float delta = normalizeDeg(toDeg) - normalizeDeg(fromDeg);
  if (delta > 180.0F) delta -= 360.0F;
  if (delta < -180.0F) delta += 360.0F;
  return delta;
}

float blendAngle(float currentDeg, float targetDeg, float alpha) {
  return normalizeDeg(currentDeg + shortestAngleDelta(currentDeg, targetDeg) * alpha);
}

float clampFloat(float value, float lo, float hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

float bearingFromVector(float east, float north) {
  return normalizeDeg(atan2f(east, north) * 57.295779513F);
}

}  // namespace

bool WindEstimator::update(float trackDeg, float groundSpeedKmh, float latitudeDeg, float longitudeDeg, uint32_t timestampMs) {
  (void)latitudeDeg;
  (void)longitudeDeg;

  if (!isfinite(trackDeg) || !isfinite(groundSpeedKmh)) {
    compute(timestampMs);
    return hasWind_;
  }

  if (groundSpeedKmh >= kMinGroundSpeedKmh) {
    const uint8_t sector = sectorForTrack(trackDeg);
    const float trackRad = trackDeg * 0.01745329252F;
    const float eastKmh = sinf(trackRad) * groundSpeedKmh;
    const float northKmh = cosf(trackRad) * groundSpeedKmh;
    SectorSample& sample = sectors_[sector];
    if (!sample.valid || !isRecent(sample, timestampMs)) {
      sample.speedKmh = groundSpeedKmh;
      sample.eastKmh = eastKmh;
      sample.northKmh = northKmh;
    } else {
      sample.speedKmh += (groundSpeedKmh - sample.speedKmh) * kSectorSpeedAlpha;
      sample.eastKmh += (eastKmh - sample.eastKmh) * kSectorSpeedAlpha;
      sample.northKmh += (northKmh - sample.northKmh) * kSectorSpeedAlpha;
    }
    sample.updatedMs = timestampMs;
    sample.valid = true;
  }

  compute(timestampMs);
  return hasWind_;
}

uint8_t WindEstimator::sectorForTrack(float trackDeg) const {
  const float normalized = normalizeDeg(trackDeg);
  int sector = static_cast<int>((normalized + kSectorWidthDeg * 0.5F) / kSectorWidthDeg);
  sector %= kSectorCount;
  if (sector < 0) sector += kSectorCount;
  return static_cast<uint8_t>(sector);
}

float WindEstimator::sectorCenterDeg(uint8_t sector) const {
  return normalizeDeg(static_cast<float>(sector % kSectorCount) * kSectorWidthDeg);
}

bool WindEstimator::isRecent(const SectorSample& sample, uint32_t nowMs) const {
  return sample.valid && sample.updatedMs != 0 && nowMs - sample.updatedMs <= kSectorTimeoutMs;
}

bool WindEstimator::sectorsAreOpposite(uint8_t a, uint8_t b) const {
  int diff = abs(static_cast<int>(a) - static_cast<int>(b));
  if (diff > kSectorCount / 2) {
    diff = kSectorCount - diff;
  }
  return diff >= 7 && diff <= 8;
}

uint8_t WindEstimator::largestRecentGap(uint32_t nowMs) const {
  uint8_t largest = 0;
  for (uint8_t start = 0; start < kSectorCount; ++start) {
    if (isRecent(sectors_[start], nowMs)) {
      continue;
    }
    uint8_t gap = 0;
    for (uint8_t offset = 0; offset < kSectorCount; ++offset) {
      const uint8_t index = static_cast<uint8_t>((start + offset) % kSectorCount);
      if (isRecent(sectors_[index], nowMs)) {
        break;
      }
      ++gap;
    }
    if (gap > largest) {
      largest = gap;
    }
  }
  return largest;
}

void WindEstimator::compute(uint32_t nowMs) {
  uint8_t recentCount = 0;
  uint8_t recentWideCount = 0;
  uint8_t minSector = 0;
  uint8_t maxSector = 0;
  float minSpeed = 9999.0F;
  float maxSpeed = -1.0F;
  float meanEastKmh = 0.0F;
  float meanNorthKmh = 0.0F;

  for (uint8_t i = 0; i < kSectorCount; ++i) {
    const SectorSample& sample = sectors_[i];
    if (!isRecent(sample, nowMs)) continue;

    ++recentCount;
    meanEastKmh += sample.eastKmh;
    meanNorthKmh += sample.northKmh;
    if (nowMs - sample.updatedMs <= kHighQualityWindowMs) {
      ++recentWideCount;
    }
    if (sample.speedKmh < minSpeed) {
      minSpeed = sample.speedKmh;
      minSector = i;
    }
    if (sample.speedKmh > maxSpeed) {
      maxSpeed = sample.speedKmh;
      maxSector = i;
    }
  }

  if (recentCount > 0) {
    meanEastKmh /= static_cast<float>(recentCount);
    meanNorthKmh /= static_cast<float>(recentCount);
  }

  if (recentCount == 0) {
    hasWind_ = false;
    windSpeedKmh_ = 0.0F;
    windDirectionFromDeg_ = 0.0F;
    windDirectionToDeg_ = 180.0F;
    quality_ = WindQuality::None;
    return;
  }

  const uint8_t maxGap = largestRecentGap(nowMs);
  const bool enoughCircleCoverage = recentCount >= 8 && maxGap <= 5;
  const bool highCircleCoverage = recentWideCount >= 12 && maxGap <= 4;
  const bool oppositePairValid = recentCount >= 2 && maxSpeed >= 0.0F && minSpeed < 9000.0F && sectorsAreOpposite(maxSector, minSector);
  if (!enoughCircleCoverage && !oppositePairValid) {
    if (hasWind_) {
      quality_ = recentCount >= 6 ? WindQuality::Medium : WindQuality::Low;
    } else {
      quality_ = recentCount == 0 ? WindQuality::None : WindQuality::Low;
      windSpeedKmh_ = 0.0F;
    }
    return;
  }

  float measuredWindSpeed = 0.0F;
  float measuredFromDeg = 0.0F;
  float measuredToDeg = 180.0F;

  if (enoughCircleCoverage) {
    measuredWindSpeed = sqrtf(meanEastKmh * meanEastKmh + meanNorthKmh * meanNorthKmh);
    measuredToDeg = bearingFromVector(meanEastKmh, meanNorthKmh);
    measuredFromDeg = normalizeDeg(measuredToDeg + 180.0F);

    if (oppositePairValid) {
      const float pairSpeed = (maxSpeed - minSpeed) * 0.5F;
      const float pairFromDeg = sectorCenterDeg(minSector);
      const float directionDelta = fabsf(shortestAngleDelta(measuredFromDeg, pairFromDeg));
      if (directionDelta <= 67.5F) {
        measuredWindSpeed = measuredWindSpeed * 0.65F + pairSpeed * 0.35F;
        measuredFromDeg = blendAngle(measuredFromDeg, pairFromDeg, 0.25F);
        measuredToDeg = normalizeDeg(measuredFromDeg + 180.0F);
      }
    }
  } else {
    measuredWindSpeed = (maxSpeed - minSpeed) * 0.5F;
    measuredFromDeg = sectorCenterDeg(minSector);
    measuredToDeg = normalizeDeg(measuredFromDeg + 180.0F);
  }

  measuredWindSpeed = clampFloat(measuredWindSpeed, 0.0F, 80.0F);
  if (measuredWindSpeed < kMinUsefulWindKmh && !hasWind_) {
    windSpeedKmh_ = 0.0F;
    quality_ = WindQuality::Low;
    return;
  }

  if (!hasWind_) {
    windSpeedKmh_ = measuredWindSpeed;
    windDirectionFromDeg_ = measuredFromDeg;
    windDirectionToDeg_ = measuredToDeg;
  } else {
    windSpeedKmh_ += (measuredWindSpeed - windSpeedKmh_) * kOutputAlpha;
    windDirectionFromDeg_ = blendAngle(windDirectionFromDeg_, measuredFromDeg, kOutputAlpha);
    windDirectionToDeg_ = normalizeDeg(windDirectionFromDeg_ + 180.0F);
  }

  hasWind_ = true;
  quality_ = highCircleCoverage ? WindQuality::High : (enoughCircleCoverage || recentCount >= 6 ? WindQuality::Medium : WindQuality::Low);
}
