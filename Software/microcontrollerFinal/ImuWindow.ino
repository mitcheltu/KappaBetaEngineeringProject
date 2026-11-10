#include "ImuWindow.h"

#include <cmath>

namespace ImuRisk {

void Window::clear() {
  sampleCount_ = 0;
  for (size_t sample = 0; sample < IMU_WINDOW_SAMPLES; ++sample) {
    valid_[sample] = false;
    timestamps_[sample] = 0;
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
      timestamps_[sample - 1] = timestamps_[sample];
      for (size_t channel = 0; channel < IMU_CHANNELS; ++channel) {
        tensor_[sample - 1][channel] = tensor_[sample][channel];
      }
    }
    destination = IMU_WINDOW_SAMPLES - 1;
  } else {
    ++sampleCount_;
  }

  timestamps_[destination] = reading.timestampUs;
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

bool Window::timingValid() const {
  if (!full()) return false;
  const uint64_t expectedIntervalUs =
      static_cast<uint64_t>(Config::IMU_PERIOD_MS) * 1000ULL;
  const uint64_t minIntervalUs = expectedIntervalUs >
          Config::IMU_SAMPLE_INTERVAL_TOLERANCE_US
      ? expectedIntervalUs - Config::IMU_SAMPLE_INTERVAL_TOLERANCE_US : 1;
  const uint64_t maxIntervalUs =
      expectedIntervalUs + Config::IMU_SAMPLE_INTERVAL_TOLERANCE_US;
  const uint64_t expectedSpanUs =
      (IMU_WINDOW_SAMPLES - 1) * expectedIntervalUs;
  if (durationUs() > expectedSpanUs + Config::IMU_WINDOW_SPAN_TOLERANCE_US)
    return false;

  for (size_t sample = 1; sample < IMU_WINDOW_SAMPLES; ++sample) {
    if (timestamps_[sample] <= timestamps_[sample - 1]) return false;
    const uint64_t intervalUs = timestamps_[sample] - timestamps_[sample - 1];
    if (intervalUs < minIntervalUs || intervalUs > maxIntervalUs)
      return false;
  }
  return true;
}

uint64_t Window::durationUs() const {
  if (!full() || timestamps_[IMU_WINDOW_SAMPLES - 1] < timestamps_[0]) return 0;
  return timestamps_[IMU_WINDOW_SAMPLES - 1] - timestamps_[0];
}

uint64_t Window::maxSampleGapUs() const {
  if (!full()) return 0;
  uint64_t largestGapUs = 0;
  for (size_t sample = 1; sample < IMU_WINDOW_SAMPLES; ++sample) {
    if (timestamps_[sample] <= timestamps_[sample - 1])
      return static_cast<uint64_t>(-1);
    const uint64_t gapUs = timestamps_[sample] - timestamps_[sample - 1];
    if (gapUs > largestGapUs) largestGapUs = gapUs;
  }
  return largestGapUs;
}

} // namespace ImuRisk
