#include "SinkRateEstimator.h"

#include <algorithm>
#include <cmath>

SinkRateEstimator::SinkRateEstimator(float totalMassKg,
                                     float displacedVolumeM3,
                                     float waterDensityKgM3,
                                     float projectedAreaM2,
                                     float gravityMps2) {
  const float netWeightMass = totalMassKg - waterDensityKgM3 * displacedVolumeM3;
  const float denominator = waterDensityKgM3 *
                            SinkRateConfig::DRAG_COEFFICIENT * projectedAreaM2;
  const bool validInputs = std::isfinite(totalMassKg) && totalMassKg > 0.0f &&
                           std::isfinite(displacedVolumeM3) && displacedVolumeM3 >= 0.0f &&
                           std::isfinite(waterDensityKgM3) && waterDensityKgM3 > 0.0f &&
                           std::isfinite(projectedAreaM2) && projectedAreaM2 > 0.0f &&
                           std::isfinite(gravityMps2) && gravityMps2 > 0.0f &&
                           netWeightMass >= 0.0f && denominator > 0.0f;
  if (!validInputs) return;

  const float squaredSpeed = (2.0f * gravityMps2 * netWeightMass) / denominator;
  if (!std::isfinite(squaredSpeed) || squaredSpeed < 0.0f) return;

  naturalSinkingSpeedMps_ = std::sqrt(squaredSpeed);
  valid_ = std::isfinite(naturalSinkingSpeedMps_) &&
           naturalSinkingSpeedMps_ > 0.0f;
}

SinkRateComparison SinkRateEstimator::compare(float measuredRateMps) const {
  SinkRateComparison result;
  result.measuredRateMps = measuredRateMps;
  result.naturalSinkingSpeedMps = naturalSinkingSpeedMps_;
  if (!valid_ || !std::isfinite(measuredRateMps)) return result;

  result.rateDifferenceMps = std::fabs(measuredRateMps - naturalSinkingSpeedMps_);
  if (!std::isfinite(result.rateDifferenceMps)) return result;

  // R_sink = max(0, 1 - |v_measured - v_natural| / v_natural).
  // Avoid an unnecessary division when the mismatch already guarantees zero.
  if (result.rateDifferenceMps >= naturalSinkingSpeedMps_) {
    result.sinkRiskScore = 0.0f;
  } else {
    result.sinkRiskScore = std::max(
        0.0f, 1.0f - result.rateDifferenceMps / naturalSinkingSpeedMps_);
  }
  result.valid = std::isfinite(result.sinkRiskScore);
  return result;
}

SinkRateComparison SinkRateEstimator::compare(const DepthReading& reading) const {
  const bool usableReading = reading.rateValid &&
                             std::isfinite(reading.depthRateMps) &&
                             (reading.qualityFlags & (DepthQuality::SENSOR_INVALID |
                                                      DepthQuality::I2C_ERROR |
                                                      DepthQuality::STALE |
                                                      DepthQuality::OUTLIER_REJECTED)) == 0;
  SinkRateComparison result = compare(reading.depthRateMps);
  if (!usableReading) {
    result.valid = false;
    result.rateDifferenceMps = NAN;
    result.sinkRiskScore = NAN;
  }
  return result;
}
