#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Ppg {

// One owner writes the ring. Copy it into a separately owned analysis buffer
// before another task consumes it; this type deliberately has no locking.
// Timestamps describe samples, not the times that a FIFO batch is serviced.
class PpgWindow {
 public:
  static constexpr std::size_t kSamples = 1600;  // 16 seconds at 100 Hz.

  explicit PpgWindow(std::uint32_t periodUs = 10000,
                     std::uint32_t toleranceUs = 2000)
      : periodUs_(periodUs), toleranceUs_(toleranceUs) {}

  void reset() {
    count_ = 0;
    next_ = 0;
    lastSampleTimeUs_ = 0;
    ++generation_;
  }

  // Returns true when the sample was accepted, not when the ring is full.
  // Invalid/nonfinite data clears the ring. A valid sample after a timestamp
  // discontinuity starts a fresh ring, so missing samples are never bridged.
  // 'valid' denotes transport/contact validity, never detected pulse presence:
  // a finite flat signal must remain eligible for pulse-loss analysis.
  bool add(float ir, std::uint64_t sampleTimeUs, bool valid) {
    if (!valid || !std::isfinite(ir) || periodUs_ == 0 ||
        toleranceUs_ >= periodUs_) {
      reset();
      return false;
    }
    if (count_ != 0) {
      const std::uint64_t delta = sampleTimeUs - lastSampleTimeUs_;
      if (sampleTimeUs <= lastSampleTimeUs_ ||
          delta < static_cast<std::uint64_t>(periodUs_ - toleranceUs_) ||
          delta > static_cast<std::uint64_t>(periodUs_) + toleranceUs_) {
        reset();
      }
    }
    samples_[next_] = ir;
    next_ = (next_ + 1) % kSamples;
    if (count_ < kSamples) ++count_;
    lastSampleTimeUs_ = sampleTimeUs;
    return true;
  }

  // Failure leaves the destination untouched. Success gives oldest-to-newest
  // data even after wraparound. The caller must supply kSamples floats.
  bool copy(float* output) const {
    if (output == nullptr || !full()) return false;
    for (std::size_t i = 0; i < kSamples; ++i)
      output[i] = samples_[(next_ + i) % kSamples];
    return true;
  }

  std::size_t count() const { return count_; }
  bool full() const { return count_ == kSamples; }
  std::uint64_t lastSampleTimeUs() const { return lastSampleTimeUs_; }
  // A caller can revoke baseline qualification when this changes. Explicit
  // contact loss or device reinitialization must also revoke qualification.
  std::uint32_t generation() const { return generation_; }

 private:
  std::array<float, kSamples> samples_{};
  const std::uint32_t periodUs_;
  const std::uint32_t toleranceUs_;
  std::size_t count_ = 0;
  std::size_t next_ = 0;
  std::uint64_t lastSampleTimeUs_ = 0;
  std::uint32_t generation_ = 0;
};

}  // namespace Ppg
