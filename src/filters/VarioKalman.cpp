#include "filters/VarioKalman.h"

#include <math.h>

void VarioKalman::reset(float altitudeM, float nowSeconds) {
  initialized_ = true;
  lastTimeSeconds_ = nowSeconds;
  altitudeM_ = altitudeM;
  varioMs_ = 0.0F;
  p00_ = 1.0F;
  p01_ = 0.0F;
  p10_ = 0.0F;
  p11_ = 1.0F;
}

bool VarioKalman::update(float measuredAltitudeM, float nowSeconds) {
  if (!initialized_) {
    reset(measuredAltitudeM, nowSeconds);
    return true;
  }

  float dt = nowSeconds - lastTimeSeconds_;
  if (dt <= 0.0F) {
    return false;
  }
  if (dt > 0.5F) {
    dt = 0.5F;
  }
  lastTimeSeconds_ = nowSeconds;

  altitudeM_ += varioMs_ * dt;

  const float dt2 = dt * dt;
  const float dt3 = dt2 * dt;
  const float dt4 = dt2 * dt2;
  const float q00 = 0.25F * dt4 * kAccelVariance;
  const float q01 = 0.5F * dt3 * kAccelVariance;
  const float q11 = dt2 * kAccelVariance;

  const float fp00 = p00_ + dt * (p10_ + p01_) + dt2 * p11_ + q00;
  const float fp01 = p01_ + dt * p11_ + q01;
  const float fp10 = p10_ + dt * p11_ + q01;
  const float fp11 = p11_ + q11;

  const float innovation = measuredAltitudeM - altitudeM_;
  const float innovationVariance = fp00 + kMeasurementVariance;
  if (innovationVariance <= 0.0F) {
    return false;
  }

  const float k0 = fp00 / innovationVariance;
  const float k1 = fp10 / innovationVariance;

  altitudeM_ += k0 * innovation;
  varioMs_ += k1 * innovation;

  p00_ = (1.0F - k0) * fp00;
  p01_ = (1.0F - k0) * fp01;
  p10_ = fp10 - k1 * fp00;
  p11_ = fp11 - k1 * fp01;

  if (fabsf(varioMs_) < 0.04F) {
    varioMs_ = 0.0F;
  }
  return true;
}
