#pragma once

#include <stdint.h>
#include <stddef.h>

namespace ImuRisk {

constexpr size_t IMU_WINDOW_SAMPLES = 60;
constexpr size_t IMU_CHANNELS = 6;

// Tensor layout: [sample][channel] = [ax, ay, az, gx, gy, gz].
using ImuTensor = float[IMU_WINDOW_SAMPLES][IMU_CHANNELS];

// TODO: replace after the CNN is trained and validated.
constexpr float TEMPORARY_RISK_THRESHOLD = 0.5f;

class Evaluator {
public:
  // Returns 1 when the unsafe score meets or exceeds the threshold; otherwise 0.
  uint8_t evaluate(const ImuTensor& tensor);

  // Returns the score produced by the most recent evaluation.
  float lastScore() const { return lastScore_; }

private:
  // Replace this body with the deployed 1D-CNN forward pass and model weights.
  float runCnn(const ImuTensor& tensor);

  float lastScore_ = 0.0f;
};

} // namespace ImuRisk
