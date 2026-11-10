#pragma once

#include <cmath>
#include <stdint.h>
#include <stddef.h>

#include "FirmwareConfig.h"

namespace ImuRisk {

constexpr size_t IMU_WINDOW_SAMPLES = Config::IMU_WINDOW_SAMPLES;
constexpr size_t IMU_CHANNELS = 6;
// Retained as the compile-time default for callers using the older name.
constexpr float TEMPORARY_RISK_THRESHOLD = Config::CNN_SCORE_THRESHOLD;

// Tensor layout: [sample][channel] = [ax, ay, az, gx, gy, gz].
using ImuTensor = float[IMU_WINDOW_SAMPLES][IMU_CHANNELS];

struct EvaluationResult {
  float score = NAN;
  uint64_t inputStartUs = 0;
  uint64_t inputEndUs = 0;
  uint8_t classification = 0;
  bool valid = false;
};

class Evaluator {
public:
  explicit Evaluator(float threshold = Config::CNN_SCORE_THRESHOLD) {
    setThreshold(threshold);
  }

  // Returns 1 when the unsafe score meets or exceeds the threshold; otherwise 0.
  uint8_t evaluate(const ImuTensor& tensor);
  uint8_t evaluate(const ImuTensor& tensor, uint64_t inputStartUs,
                   uint64_t inputEndUs);

  // Returns the score produced by the most recent evaluation.
  float lastScore() const { return result_.score; }
  const EvaluationResult& result() const { return result_; }
  float threshold() const { return threshold_; }
  bool setThreshold(float threshold);

private:
  // Replace this body with the deployed 1D-CNN forward pass and model weights.
  float runCnn(const ImuTensor& tensor);

  float threshold_ = Config::CNN_SCORE_THRESHOLD;
  EvaluationResult result_{};
};

} // namespace ImuRisk
