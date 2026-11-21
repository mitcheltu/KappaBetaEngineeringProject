#pragma once
#include <cmath>
#include <stddef.h>
#include "ImuCnnParams.h"

// Hand-written forward pass of the teammate's IMU CNN (imu_cnn_bce.tflite), exported by
// Software/testsAndDemo/scenarios/export_imu_cnn.py. No TensorFlow Lite Micro needed.
//
//   input  60 x 6 (3 s at 20 Hz), ax ay az [m/s^2], gx gy gz [rad/s]
//   normalize (x - Mean) / Scale
//   Conv1D 6->32, k5, SAME, ReLU      -> 60 x 32
//   MaxPool 2                         -> 30 x 32
//   Conv1D 32->64, k3, SAME, ReLU     -> 30 x 64
//   mean over time                    -> 64
//   Dense 64->1, sigmoid              -> score
//
// The BMI270 driver reports g and degrees/s: convert before calling score()
// (see toModelUnits). About 6 KB of static workspace; keep the object off small stacks.
namespace Imu {
class Cnn {
 public:
  static constexpr int Samples = 60, Axes = 6;

  // window: [Samples][Axes] in model units. Returns NaN on non-finite input.
  float score(const float* window) {
    using namespace CnnParams;
    for (int t = 0; t < Samples; ++t)
      for (int a = 0; a < Axes; ++a) {
        const float v = window[t * Axes + a];
        if (!std::isfinite(v)) return NAN;
        x_[t][a] = (v - Mean[a]) / Scale[a];
      }
    // Conv1 (SAME: 2 taps of zero padding each side) + ReLU, fused with MaxPool(2).
    for (int t = 0; t < Samples / 2; ++t)
      for (int o = 0; o < 32; ++o) {
        float pooled = 0.0f;  // ReLU floor
        for (int q = 0; q < 2; ++q) {
          const int centre = 2 * t + q;
          float value = Conv1Bias[o];
          for (int k = 0; k < 5; ++k) {
            const int i = centre + k - 2;
            if (i < 0 || i >= Samples) continue;
            const float* w = Conv1Weight + (o * 5 + k) * Axes;
            for (int a = 0; a < Axes; ++a) value += w[a] * x_[i][a];
          }
          if (value > pooled) pooled = value;
        }
        p_[t][o] = pooled;
      }
    // Conv2 (SAME: 1 tap each side) + ReLU, then the time mean.
    float mean[64];
    for (int o = 0; o < 64; ++o) {
      float sum = 0.0f;
      for (int t = 0; t < Samples / 2; ++t) {
        float value = Conv2Bias[o];
        for (int k = 0; k < 3; ++k) {
          const int i = t + k - 1;
          if (i < 0 || i >= Samples / 2) continue;
          const float* w = Conv2Weight + (o * 3 + k) * 32;
          for (int c = 0; c < 32; ++c) value += w[c] * p_[i][c];
        }
        sum += value > 0.0f ? value : 0.0f;
      }
      mean[o] = sum / (Samples / 2);
    }
    float logit = DenseBias[0];
    for (int o = 0; o < 64; ++o) logit += DenseWeight[o] * mean[o];
    if (!std::isfinite(logit)) return NAN;
    return logit >= 0 ? 1.0f / (1.0f + std::exp(-logit)) : std::exp(logit) / (1.0f + std::exp(logit));
  }

  static bool gate(float score) { return std::isfinite(score) && score >= CnnParams::GateThreshold; }

  // BMI270 driver units (g, degrees/s) -> model units (m/s^2, rad/s).
  static void toModelUnits(const float in[Axes], float out[Axes]) {
    constexpr float G = 9.80665f, DegToRad = 0.017453292519943295f;
    for (int a = 0; a < 3; ++a) out[a] = in[a] * G;
    for (int a = 3; a < 6; ++a) out[a] = in[a] * DegToRad;
  }

 private:
  float x_[Samples][Axes] = {};
  float p_[Samples / 2][32] = {};
};
}  // namespace Imu
