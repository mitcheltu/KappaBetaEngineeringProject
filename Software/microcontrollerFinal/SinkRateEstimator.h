#pragma once

#include "DepthSensor.h"

struct SinkRateComparison {
  float measuredRateMps = NAN;
  float naturalSinkingSpeedMps = NAN;
  float rateDifferenceMps = NAN;
  bool valid = false;
};

class SinkRateEstimator {
 public:
  // Profile values are supplied by the caller. Height/profile collection and
  // displaced-volume estimation remain outside this class and DepthSensor.
  SinkRateEstimator(float totalMassKg, float displacedVolumeM3,
                    float waterDensityKgM3, float dragCoefficient,
                    float projectedAreaM2, float gravityMps2 = 9.81f);

  bool valid() const { return valid_; }
  float naturalSinkingSpeedMps() const { return naturalSinkingSpeedMps_; }
  SinkRateComparison compare(float measuredRateMps) const;
  SinkRateComparison compare(const DepthReading& reading) const;

 private:
  float naturalSinkingSpeedMps_ = NAN;
  bool valid_ = false;
};
