#pragma once

class VarioKalman {
 public:
  void reset(float altitudeM, float nowSeconds);
  bool update(float measuredAltitudeM, float nowSeconds);

  float altitudeM() const { return altitudeM_; }
  float varioMs() const { return varioMs_; }
  bool initialized() const { return initialized_; }

 private:
  static constexpr float kMeasurementVariance = 0.36F;
  static constexpr float kAccelVariance = 1.44F;

  bool initialized_ = false;
  float lastTimeSeconds_ = 0.0F;
  float altitudeM_ = 0.0F;
  float varioMs_ = 0.0F;
  float p00_ = 1.0F;
  float p01_ = 0.0F;
  float p10_ = 0.0F;
  float p11_ = 1.0F;
};
