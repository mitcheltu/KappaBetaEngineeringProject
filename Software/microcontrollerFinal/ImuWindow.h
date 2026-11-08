#pragma once

#include <cstddef>

#include "ImuRiskEvaluator.h"
#include "ImuSensor.h"

namespace ImuRisk {

// Fixed-rate acquisition window. The buffer holds exactly three seconds at
// the configured 20 Hz sample rate. Invalid reads occupy a slot rather than
// being skipped, so the time span remains honest.
class Window {
 public:
  Window() { clear(); }

  void clear();
  void push(const ImuReading& reading);

  bool full() const { return sampleCount_ == IMU_WINDOW_SAMPLES; }
  bool allValid() const;
  size_t sampleCount() const { return sampleCount_; }
  const ImuTensor& tensor() const { return tensor_; }

 private:
  ImuTensor tensor_{};
  bool valid_[IMU_WINDOW_SAMPLES]{};
  size_t sampleCount_ = 0;
};

} // namespace ImuRisk
