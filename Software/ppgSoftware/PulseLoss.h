#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include "PulseLossParams.h"

namespace Ppg {

enum class PulseModel : uint8_t { Rule, Logistic, BoostedTrees, Cnn };
enum class PulseOperatingPoint : uint8_t { HighSensitivity, HighSpecificity };

struct PulseResult {
  bool valid = false;
  bool alert = false;
  bool ambiguousPeakTies = false;
  double features[10] = {};
  double score = std::numeric_limits<double>::quiet_NaN();
  double threshold = std::numeric_limits<double>::quiet_NaN();
  std::size_t peakCount = 0;
};

// Single-owner, fixed-size workspace. Put this object in static storage, never
// on the ESP32 task stack. Input is a coherent, chronological 16 s / 100 Hz copy.
// This computes research scores only: the caller owns quality gating and alerts.
class PulseLoss {
 public:
  static constexpr std::size_t SampleCount = 1600;
  static constexpr std::size_t FeatureCount = 10;
  static constexpr std::size_t SampleHz = 100;
  static constexpr std::size_t MaxPeaks = SampleCount / 2;

  PulseResult evaluate(const float* raw, PulseModel model = PulseModel::Rule,
                       PulseOperatingPoint point = PulseOperatingPoint::HighSpecificity) {
    PulseResult result;
    peakCount_ = 0;
    ambiguousPeakTies_ = false;
    if (raw == nullptr || !available(model) || static_cast<unsigned>(point) > 1) return result;
    for (std::size_t i = 0; i < SampleCount; ++i) {
      if (!std::isfinite(raw[i])) return result;
      scratch_[i] = raw[i];
    }
    const double median = percentile(scratch_, SampleCount, 0.5);
    for (std::size_t i = 0; i < SampleCount; ++i) filtered_[Pad + i] = double(raw[i]) - median;
    // scipy.signal.sosfiltfilt's default odd extension and steady-state zi.
    for (std::size_t i = 0; i < Pad; ++i) {
      filtered_[i] = 2 * filtered_[Pad] - filtered_[2 * Pad - i];
      filtered_[Pad + SampleCount + i] = 2 * filtered_[Pad + SampleCount - 1]
                                        - filtered_[Pad + SampleCount - 2 - i];
    }
    filterPass();
    std::reverse(filtered_, filtered_ + Extended);
    filterPass();
    std::reverse(filtered_, filtered_ + Extended);
    double* z = filtered_ + Pad;
    std::copy(z, z + SampleCount, scratch_);
    const double p95 = percentile(scratch_, SampleCount, 0.95);
    const double p05 = percentile(scratch_, SampleCount, 0.05);
    const double spread = p95 - p05;
    bool nonzero = false;
    for (std::size_t i = 0; i < SampleCount; ++i) {
      z[i] = spread > 0 ? z[i] / spread : 0;
      if (!std::isfinite(z[i])) return result;
      nonzero = nonzero || z[i] != 0;
    }
    if (nonzero) {
      result.peakCount = detectPeaks(z, SampleCount);
      // NumPy's unstable argsort does not specify which equal-height peak wins
      // the distance constraint. Abstain instead of scoring a possibly different
      // feature vector from training. This is exact equality, not an epsilon.
      result.ambiguousPeakTies = ambiguousPeakTies_;
      if (result.ambiguousPeakTies) return result;
      computeFeatures(z, result.features);
    } else {
      peakCount_ = 0;
      result.features[0] = result.features[1] = 16;
      result.features[8] = 1;
    }
    for (double feature : result.features) if (!std::isfinite(feature)) return result;
    result.score = score(result.features, z, model);
    result.threshold = PulseParams::Thresholds[static_cast<unsigned>(model) * 2 + static_cast<unsigned>(point)];
    result.valid = std::isfinite(result.score);
    result.alert = result.valid && result.score >= result.threshold;
    return result;
  }

  static bool available(PulseModel model) {
    switch (model) {
      case PulseModel::Rule: return true;
      case PulseModel::Logistic: return PulseParams::HasLogistic;
      case PulseModel::BoostedTrees: return PulseParams::HasBoostedTrees;
      case PulseModel::Cnn: return PulseParams::HasCnn;
    }
    return false;
  }

  // Exposed to check the prominence/distance port independently against SciPy.
  // Equal-height conflicts use the rightmost peak, as in the reference fixtures.
  std::size_t detectPeaks(const double* z, std::size_t n, double prominence = 0.3,
                          std::size_t distance = 30) {
    peakCount_ = 0;
    ambiguousPeakTies_ = false;
    if (z == nullptr || n > SampleCount || n < 3 || distance < 1) return 0;
    for (std::size_t i = 1; i + 1 < n;) {
      if (z[i - 1] < z[i]) {
        std::size_t right = i;
        while (right + 1 < n && z[right + 1] == z[i]) ++right;
        if (right + 1 < n && z[right + 1] < z[i]) {
          peaks_[peakCount_] = static_cast<uint16_t>((i + right) / 2);
          order_[peakCount_] = static_cast<uint16_t>(peakCount_);
          keep_[peakCount_] = true;
          ++peakCount_;
        }
        i = right + 1;
      } else ++i;
    }
    std::sort(order_, order_ + peakCount_, [this, z](uint16_t a, uint16_t b) {
      return z[peaks_[a]] < z[peaks_[b]] || (z[peaks_[a]] == z[peaks_[b]] && peaks_[a] < peaks_[b]);
    });
    // Distance filtering precedes prominence filtering in scipy.find_peaks.
    for (std::size_t rank = peakCount_; rank > 0; --rank) {
      const std::size_t k = order_[rank - 1];
      if (!keep_[k]) continue;
      for (std::size_t j = k; j > 0;) {
        --j;
        if (std::size_t(peaks_[k] - peaks_[j]) >= distance) break;
        if (keep_[j] && z[peaks_[j]] == z[peaks_[k]]) ambiguousPeakTies_ = true;
        keep_[j] = false;
      }
      for (std::size_t j = k + 1; j < peakCount_; ++j) {
        if (std::size_t(peaks_[j] - peaks_[k]) >= distance) break;
        if (keep_[j] && z[peaks_[j]] == z[peaks_[k]]) ambiguousPeakTies_ = true;
        keep_[j] = false;
      }
    }
    std::size_t out = 0;
    for (std::size_t k = 0; k < peakCount_; ++k) {
      if (!keep_[k]) continue;
      const std::size_t p = peaks_[k];
      double leftMin = z[p], rightMin = z[p];
      for (std::size_t i = p; ; --i) {
        if (z[i] > z[p]) break;
        leftMin = std::min(leftMin, z[i]);
        if (i == 0) break;
      }
      for (std::size_t i = p; i < n && z[i] <= z[p]; ++i) rightMin = std::min(rightMin, z[i]);
      const double prom = z[p] - std::max(leftMin, rightMin);
      if (prom >= prominence) {
        peaks_[out] = static_cast<uint16_t>(p);
        prominences_[out] = prom;
        ++out;
      }
    }
    peakCount_ = out;
    return out;
  }

  std::size_t peakIndex(std::size_t index) const { return index < peakCount_ ? peaks_[index] : SampleCount; }
  double peakProminence(std::size_t index) const {
    return index < peakCount_ ? prominences_[index] : std::numeric_limits<double>::quiet_NaN();
  }
  const double* prepared() const { return filtered_ + Pad; }
  bool ambiguousPeakTies() const { return ambiguousPeakTies_; }

 private:
  static constexpr std::size_t Pad = 21;
  static constexpr std::size_t Extended = SampleCount + 2 * Pad;
  static constexpr std::size_t FftSize = 512;
  static constexpr double Pi = 3.1415926535897932384626433832795;
  double filtered_[Extended] = {};
  double scratch_[SampleCount] = {};
  double prominences_[MaxPeaks] = {};
  uint16_t peaks_[MaxPeaks] = {}, order_[MaxPeaks] = {};
  bool keep_[MaxPeaks] = {};
  std::size_t peakCount_ = 0;
  bool ambiguousPeakTies_ = false;
  // Separate storage keeps both array lifetimes unambiguous in C++17.
  struct Work {
    struct Spectral { double real[FftSize], imag[FftSize], power[FftSize / 2 + 1]; } spectral{};
    struct Cnn { float first[8 * 400], second[16 * 100]; } cnn{};
  } work_;

  static double percentile(double* values, std::size_t n, double quantile) {
    if (n == 0) return 0;
    const double position = (n - 1) * quantile;
    const std::size_t low = static_cast<std::size_t>(position);
    std::nth_element(values, values + low, values + n);
    const double a = values[low];
    if (low + 1 == n || position == double(low)) return a;
    std::nth_element(values + low + 1, values + low + 1, values + n);
    const double b = values[low + 1];
    return a + (position - low) * (b - a);
  }

  void filterPass() {
    const double initial = filtered_[0];
    double state[3][2];
    for (std::size_t s = 0; s < 3; ++s) {
      state[s][0] = PulseParams::Zi[s * 2] * initial;
      state[s][1] = PulseParams::Zi[s * 2 + 1] * initial;
    }
    for (std::size_t i = 0; i < Extended; ++i) {
      double x = filtered_[i];
      for (std::size_t s = 0; s < 3; ++s) {
        const double* c = PulseParams::Sos + s * 6;
        const double y = c[0] * x + state[s][0];
        state[s][0] = c[1] * x - c[4] * y + state[s][1];
        state[s][1] = c[2] * x - c[5] * y;
        x = y;
      }
      filtered_[i] = x;
    }
  }

  void computeFeatures(const double* z, double* f) {
    std::size_t maxGap = 0, previous = 0;
    for (std::size_t i = 0; i < peakCount_; ++i) {
      maxGap = std::max(maxGap, std::size_t(peaks_[i]) - previous);
      previous = peaks_[i];
    }
    const std::size_t tailGap = SampleCount - previous;
    f[0] = std::max(maxGap, tailGap) / 100.0;
    f[1] = tailGap / 100.0;
    f[2] = double(peakCount_);
    const std::size_t intervals = peakCount_ > 0 ? peakCount_ - 1 : 0;
    if (intervals) {
      double ibiSum = 0;
      for (std::size_t i = 0; i < intervals; ++i) {
        const double ibi = (peaks_[i + 1] - peaks_[i]) / 100.0;
        scratch_[i] = 60 / ibi;
        ibiSum += ibi;
      }
      f[3] = percentile(scratch_, intervals, 0.5);
      if (intervals >= 5) {
        double minimum = std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i + 5 <= intervals; ++i) {
          const double meanIbi = (peaks_[i + 5] - peaks_[i]) / 500.0;
          minimum = std::min(minimum, 60 / meanIbi);
        }
        f[4] = minimum;
      } else f[4] = *std::min_element(scratch_, scratch_ + intervals);
      if (intervals > 1) {
        const double mean = ibiSum / intervals;
        double variance = 0;
        for (std::size_t i = 0; i < intervals; ++i) {
          const double delta = (peaks_[i + 1] - peaks_[i]) / 100.0 - mean;
          variance += delta * delta;
        }
        f[5] = std::sqrt(variance / intervals) / mean;
      }
    }
    std::size_t count = 0;
    for (std::size_t i = 0; i < peakCount_; ++i) if (peaks_[i] < 800) scratch_[count++] = prominences_[i];
    const double head = count ? percentile(scratch_, count, 0.5) : 0;
    count = 0;
    for (std::size_t i = 0; i < peakCount_; ++i) if (peaks_[i] > 1200) scratch_[count++] = prominences_[i];
    f[6] = head > 0 && count ? percentile(scratch_, count, 0.5) / head : 0;
    f[7] = spectralPurity(z);
    std::size_t flat = 0;
    double mean = 0;
    for (std::size_t i = 0; i < SampleCount; ++i) {
      mean += z[i];
      if (i && std::abs(z[i] - z[i - 1]) < 0.002) ++flat;
    }
    f[8] = double(flat) / (SampleCount - 1);
    mean /= SampleCount;
    double m2 = 0, m4 = 0;
    for (std::size_t i = 0; i < SampleCount; ++i) {
      const double d = z[i] - mean, square = d * d;
      m2 += square;
      m4 += square * square;
    }
    m2 /= SampleCount;
    m4 /= SampleCount;
    f[9] = m2 > 0 ? m4 / (m2 * m2) - 3 : std::numeric_limits<double>::quiet_NaN();
  }

  void fft() {
    auto& w = work_.spectral;
    for (std::size_t i = 1, j = 0; i < FftSize; ++i) {
      std::size_t bit = FftSize >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) { std::swap(w.real[i], w.real[j]); std::swap(w.imag[i], w.imag[j]); }
    }
    for (std::size_t length = 2; length <= FftSize; length <<= 1) {
      const double angle = -2 * Pi / length;
      const double stepR = std::cos(angle), stepI = std::sin(angle);
      for (std::size_t start = 0; start < FftSize; start += length) {
        double wr = 1, wi = 0;
        for (std::size_t j = 0; j < length / 2; ++j) {
          const std::size_t a = start + j, b = a + length / 2;
          const double r = wr * w.real[b] - wi * w.imag[b];
          const double im = wr * w.imag[b] + wi * w.real[b];
          w.real[b] = w.real[a] - r; w.imag[b] = w.imag[a] - im;
          w.real[a] += r; w.imag[a] += im;
          const double next = wr * stepR - wi * stepI;
          wi = wr * stepI + wi * stepR; wr = next;
        }
      }
    }
  }

  double spectralPurity(const double* z) {
    auto& w = work_.spectral;
    std::fill(w.power, w.power + FftSize / 2 + 1, 0);
    // scipy.signal.welch defaults: periodic Hann, mean detrend, 50% overlap,
    // averaging of complete segments; common PSD factors cancel in this ratio.
    for (std::size_t start = 0; start + FftSize <= SampleCount; start += FftSize / 2) {
      double mean = 0;
      for (std::size_t i = 0; i < FftSize; ++i) mean += z[start + i];
      mean /= FftSize;
      for (std::size_t i = 0; i < FftSize; ++i) {
        w.real[i] = (z[start + i] - mean) * (0.5 - 0.5 * std::cos(2 * Pi * i / FftSize));
        w.imag[i] = 0;
      }
      fft();
      for (std::size_t i = 0; i <= FftSize / 2; ++i)
        w.power[i] += w.real[i] * w.real[i] + w.imag[i] * w.imag[i];
    }
    constexpr std::size_t low = 3, high = 20; // bins within 0.5..4 Hz at 100/512 Hz.
    std::size_t maximum = low;
    double total = 0;
    for (std::size_t i = low; i <= high; ++i) {
      total += w.power[i];
      if (w.power[i] > w.power[maximum]) maximum = i;
    }
    double near = 0;
    for (std::size_t i = low; i <= high; ++i)
      if (std::abs(double(i) - double(maximum)) * 100 / FftSize <= 0.25) near += w.power[i];
    // Reference floors the *scaled density*, not raw FFT energy, at 1e-12.
    // Five segments, periodic Hann squared sum = 192, two-sided-to-one-sided x2.
    const double scale = 2.0 / (5 * 100 * 192);
    return near * scale / std::max(total * scale, 1e-12);
  }

  static double sigmoid(double x) {
    if (x >= 0) return 1 / (1 + std::exp(-x));
    const double e = std::exp(x);
    return e / (1 + e);
  }

  double score(const double* f, const double* z, PulseModel model) {
    using namespace PulseParams;
    if (model == PulseModel::Rule) return f[0] >= 4 || (f[4] < 40 && f[2] > 0) ? 1 : 0;
    if (model == PulseModel::Logistic) {
      double sum = LogIntercept;
      for (std::size_t i = 0; i < FeatureCount; ++i) sum += (f[i] - LogMean[i]) / LogScale[i] * LogCoefficient[i];
      return sigmoid(sum);
    }
    if (model == PulseModel::BoostedTrees) {
      double sum = TreeBaseline;
      for (std::size_t t = 0; t < TreeCount; ++t) {
        const TreeNode* tree = TreeNodes + TreeOffsets[t];
        std::size_t node = 0;
        while (!tree[node].leaf) {
          const auto& n = tree[node];
          const double value = f[n.feature];
          node = (std::isnan(value) ? n.missingLeft : value <= n.threshold) ? n.left : n.right;
        }
        // HistGradientBoosting stores learning-rate-scaled leaf values.
        sum += tree[node].value;
      }
      return sigmoid(sum);
    }
    return cnnScore(z);
  }

  double cnnScore(const double* z) {
    using namespace PulseParams;
    auto& w = work_.cnn;
    // Fuse ReLU/max-pool into each strided convolution, matching PyTorch's
    // zero padding. Tensor storage is [channel][time], weights [out][in][tap].
    for (std::size_t c = 0; c < 8; ++c) {
      for (std::size_t t = 0; t < 400; ++t) {
        float maximum = 0;
        for (std::size_t p = 0; p < 2; ++p) {
          float value = Conv1Bias[c];
          for (int k = 0; k < 7; ++k) {
            const int i = int((t * 2 + p) * 2) + k - 3;
            if (i >= 0 && i < int(SampleCount)) value += Conv1Weight[c * 7 + k] * static_cast<float>(z[i]);
          }
          maximum = std::max(maximum, value);
        }
        w.first[c * 400 + t] = maximum;
      }
    }
    for (std::size_t c = 0; c < 16; ++c) {
      for (std::size_t t = 0; t < 100; ++t) {
        float maximum = 0;
        for (std::size_t p = 0; p < 2; ++p) {
          float value = Conv2Bias[c];
          for (std::size_t in = 0; in < 8; ++in) for (int k = 0; k < 7; ++k) {
            const int i = int((t * 2 + p) * 2) + k - 3;
            if (i >= 0 && i < 400) value += Conv2Weight[(c * 8 + in) * 7 + k] * w.first[in * 400 + i];
          }
          maximum = std::max(maximum, value);
        }
        w.second[c * 100 + t] = maximum;
      }
    }
    float logit = HeadBias[0];
    for (std::size_t c = 0; c < 32; ++c) {
      float average = 0;
      for (std::size_t t = 0; t < 100; ++t) {
        float value = Conv3Bias[c];
        for (std::size_t in = 0; in < 16; ++in) for (int k = 0; k < 5; ++k) {
          const int i = int(t) + k - 2;
          if (i >= 0 && i < 100) value += Conv3Weight[(c * 16 + in) * 5 + k] * w.second[in * 100 + i];
        }
        average += std::max(0.0f, value);
      }
      logit += HeadWeight[c] * (average / 100);
    }
    return sigmoid(logit);
  }
};
} // namespace Ppg
