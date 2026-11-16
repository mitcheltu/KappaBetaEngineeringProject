#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include "PulseCnnV5Params.h"

namespace Ppg {

// Inference-only port of notebook 04 v5 `PulseCNN5`, exported as the cnn_v5_h
// three-member ensemble (PyTorch eval mode: dropout off, mean of member sigmoids).
//
//   input  1 x 1600  prepared window (0.5-8 Hz band-pass, 5-95 % spread scaling)
//   Conv(1->8,  k9, stride 2, pad 4)          + ReLU + MaxPool(2) ->  8 x 400
//   Conv(8->16, k7, pad 3)                    + ReLU + MaxPool(2) -> 16 x 200
//   Conv(16->16, k7, pad 3)                   + ReLU + MaxPool(2) -> 16 x 100
//   Conv(16->24, k5, pad 4, dilation 2)       + ReLU + MaxPool(2) -> 24 x 50
//   Conv(24->24, k5, pad 4, dilation 2)       + ReLU + AdaptiveMaxPool(8) -> 24 x 8
//   Linear(192 -> 1), channel-major flatten
//
// Storage is [channel][time]; weights are [out][in][tap]. ReLU followed by max
// pooling equals max(0, conv...), so both are fused into each convolution.
// The ~4.9 s receptive field covers the 4 s asystole definition, and the eight
// ~2 s bins keep coarse position (for example, beats stopping near the end).
// This object is ~26 KB: give it static storage, never the ESP32 task stack.
// It computes a research score only; it owns no alerting or actuation.
class PulseCnnV5 {
 public:
  static constexpr std::size_t InputCount = 1600;
  static constexpr std::size_t Members = PulseCnnV5Params::Members;

  static constexpr bool available() { return PulseCnnV5Params::Available; }

  // z: chronological prepared samples (PulseLoss::prepared()). Returns the
  // ensemble probability, or NaN if unavailable or any value is non-finite.
  double probability(const double* z) {
    if (!available() || z == nullptr) return std::numeric_limits<double>::quiet_NaN();
    double sum = 0;
    for (std::size_t m = 0; m < Members; ++m) {
      member_[m] = memberProbability(z, m);
      if (!std::isfinite(member_[m])) return std::numeric_limits<double>::quiet_NaN();
      sum += member_[m];
    }
    return sum / Members;
  }

  // Valid after probability(); exposed for parity tests and diagnostics.
  double lastMember(std::size_t m) const {
    return m < Members ? member_[m] : std::numeric_limits<double>::quiet_NaN();
  }

 private:
  using P = int;  // signed indices make zero padding explicit
  float a_[8 * 400] = {};   // ping: 8x400, then 16x100
  float b_[16 * 200] = {};  // pong: 16x200, then 24x50
  double member_[Members] = {};

  // One Conv1d (PyTorch cross-correlation, zero padding) with fused ReLU and
  // MaxPool1d(2). outLen is the pooled length; the conv emits 2*outLen values.
  template <typename Input>
  static void convReluPool2(const Input* x, P inChannels, P inLength, const float* weight,
                            const float* bias, P outChannels, P taps, P stride, P dilation,
                            P padding, float* y, P outLength) {
    for (P c = 0; c < outChannels; ++c) {
      const float* w = weight + c * inChannels * taps;
      for (P t = 0; t < outLength; ++t) {
        float pooled = 0.0f;  // ReLU floor
        for (P q = 0; q < 2; ++q) {
          const P origin = (2 * t + q) * stride - padding;
          float value = bias[c];
          for (P in = 0; in < inChannels; ++in) {
            const Input* row = x + in * inLength;
            const float* kernel = w + in * taps;
            for (P k = 0; k < taps; ++k) {
              const P i = origin + k * dilation;
              if (i >= 0 && i < inLength) value += kernel[k] * static_cast<float>(row[i]);
            }
          }
          pooled = std::max(pooled, value);
        }
        y[c * outLength + t] = pooled;
      }
    }
  }

  double memberProbability(const double* z, std::size_t m) {
    using namespace PulseCnnV5Params;
    const P M = static_cast<P>(m);
    convReluPool2(z, 1, 1600, Conv1Weight + M * 8 * 1 * 9, Conv1Bias + M * 8, 8, 9, 2, 1, 4, a_, 400);
    convReluPool2(a_, 8, 400, Conv2Weight + M * 16 * 8 * 7, Conv2Bias + M * 16, 16, 7, 1, 1, 3, b_, 200);
    convReluPool2(b_, 16, 200, Conv3Weight + M * 16 * 16 * 7, Conv3Bias + M * 16, 16, 7, 1, 1, 3, a_, 100);
    convReluPool2(a_, 16, 100, Conv4Weight + M * 24 * 16 * 5, Conv4Bias + M * 24, 24, 5, 1, 2, 4, b_, 50);

    // Conv5 (length 50 -> 50) + ReLU + AdaptiveMaxPool1d(8): bin i spans
    // [floor(i*50/8), ceil((i+1)*50/8)), PyTorch's adaptive window rule.
    const float* w5 = Conv5Weight + M * 24 * 24 * 5;
    const float* b5 = Conv5Bias + M * 24;
    const float* head = HeadWeight + M * 192;
    float logit = HeadBias[m];
    for (P c = 0; c < 24; ++c) {
      float conv[50];
      for (P t = 0; t < 50; ++t) {
        float value = b5[c];
        for (P in = 0; in < 24; ++in) {
          const float* kernel = w5 + (c * 24 + in) * 5;
          for (P k = 0; k < 5; ++k) {
            const P i = t + 2 * k - 4;
            if (i >= 0 && i < 50) value += kernel[k] * b_[in * 50 + i];
          }
        }
        conv[t] = value;
      }
      for (P bin = 0; bin < 8; ++bin) {
        const P begin = (bin * 50) / 8, end = ((bin + 1) * 50 + 7) / 8;
        float pooled = 0.0f;
        for (P t = begin; t < end; ++t) pooled = std::max(pooled, conv[t]);
        logit += head[c * 8 + bin] * pooled;
      }
    }
    if (!std::isfinite(logit)) return std::numeric_limits<double>::quiet_NaN();
    return logit >= 0 ? 1.0 / (1.0 + std::exp(-double(logit)))
                      : std::exp(double(logit)) / (1.0 + std::exp(double(logit)));
  }
};

}  // namespace Ppg
