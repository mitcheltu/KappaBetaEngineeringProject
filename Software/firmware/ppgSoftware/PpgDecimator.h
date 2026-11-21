#pragma once
#include <stdint.h>

// Averages consecutive raw MAX30102 samples into the cadence expected by the
// SparkFun/Maxim HR/SpO2 routine (FS = 25 Hz, hard-coded in spo2_algorithm.h).
// Raw FIFO samples arrive at Config::PPG_SAMPLE_HZ with on-chip averaging off;
// averaging Factor adjacent samples reproduces the former on-chip SMP_AVE=4
// output, so the HR/SpO2 estimates keep their previous input cadence.
// Pure single-owner logic: no Arduino dependency, host-testable.
template <uint8_t Factor>
class PpgDecimator {
  static_assert(Factor >= 1, "Decimation factor must be positive");

 public:
  void reset() {
    count_ = 0;
    redSum_ = irSum_ = 0;
  }

  // Returns true when Factor samples have been combined into one output sample.
  bool add(uint32_t red, uint32_t ir, uint32_t& redOut, uint32_t& irOut) {
    redSum_ += red;
    irSum_ += ir;
    if (++count_ < Factor) return false;
    redOut = static_cast<uint32_t>(redSum_ / Factor);
    irOut = static_cast<uint32_t>(irSum_ / Factor);
    reset();
    return true;
  }

  uint8_t pending() const { return count_; }

 private:
  uint64_t redSum_ = 0, irSum_ = 0;
  uint8_t count_ = 0;
};
