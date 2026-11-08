#include "ImuWindow.h"

#include <cmath>

namespace ImuRisk {

void Window::clear() {
  sampleCount_ = 0;
  for (size_t sample = 0; sample < IMU_WINDOW_SAMPLES; ++sample) {
    valid_[sample] = false;
    for (size_t channel = 0; channel < IMU_CHANNELS; ++channel) {
      tensor_[sample][channel] = NAN;
    }
  }
}

void Window::push(const ImuReading& reading) {
  size_t destination = sampleCount_;
  if (full()) {
    for (size_t sample = 1; sample < IMU_WINDOW_SAMPLES; ++sample) {
      valid_[sample - 1] = valid_[sample];
      for (size_t channel = 0; channel < IMU_CHANNELS; ++channel) {
        tensor_[sample - 1][channel] = tensor_[sample][channel];
      }
    }
    destination = IMU_WINDOW_SAMPLES - 1;
  } else {
    ++sampleCount_;
  }

  const bool valid = reading.valid &&
                     std::isfinite(reading.acceleration[0]) &&
                     std::isfinite(reading.acceleration[1]) &&
                     std::isfinite(reading.acceleration[2]) &&
                     std::isfinite(reading.gyro[0]) &&
                     std::isfinite(reading.gyro[1]) &&
                     std::isfinite(reading.gyro[2]);
  valid_[destination] = valid;
  if (valid) {
    tensor_[destination][0] = reading.acceleration[0];
    tensor_[destination][1] = reading.acceleration[1];
    tensor_[destination][2] = reading.acceleration[2];
    tensor_[destination][3] = reading.gyro[0];
    tensor_[destination][4] = reading.gyro[1];
    tensor_[destination][5] = reading.gyro[2];
  } else {
    for (size_t channel = 0; channel < IMU_CHANNELS; ++channel) {
      tensor_[destination][channel] = NAN;
    }
  }
}

bool Window::allValid() const {
  if (!full()) return false;
  for (const bool valid : valid_) {
    if (!valid) return false;
  }
  return true;
}

} // namespace ImuRisk
