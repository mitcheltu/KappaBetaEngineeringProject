#include "ImuSensor.h"

#include <cmath>
#include <esp_timer.h>

bool ImuSensor::begin(TwoWire& bus, uint8_t address) {
  invalidateReading();
  gyroCalibrationActive_ = false;
  ready_ = driver_.begin(bus, address);
  return ready_;
}

bool ImuSensor::update() {
  float rawAxes[6] = {};
  const bool transferOk = ready_ && driver_.read(rawAxes);
  const uint64_t sampleTimeUs = static_cast<uint64_t>(esp_timer_get_time());
  const uint32_t sampleTimeMs = millis();
  if (!transferOk) {
    ready_ = false;
    invalidateReading();
    // Retain when the failed sample slot was attempted so the rolling window
    // can distinguish a timed invalid read from an absent sample.
    reading_.timestampUs = sampleTimeUs;
    reading_.timestampMs = sampleTimeMs;
    return false;
  }

  applyCalibration(rawAxes);
  reading_.timestampUs = sampleTimeUs;
  reading_.timestampMs = sampleTimeMs;
  reading_.valid = true;
  return true;
}

bool ImuSensor::calibrateGyro(uint16_t sampleCount, uint16_t intervalMs) {
  if (!ready_ || sampleCount == 0) return false;
  gyroCalibrationActive_ = false;

  double gyroSums[3] = {0.0, 0.0, 0.0};
  for (uint16_t sample = 0; sample < sampleCount; ++sample) {
    float rawAxes[6] = {};
    if (!driver_.read(rawAxes) ||
        !std::isfinite(rawAxes[3]) ||
        !std::isfinite(rawAxes[4]) ||
        !std::isfinite(rawAxes[5])) {
      ready_ = false;
      invalidateReading();
      return false;
    }

    gyroSums[0] += rawAxes[3];
    gyroSums[1] += rawAxes[4];
    gyroSums[2] += rawAxes[5];
    if (sample + 1 < sampleCount) delay(intervalMs);
  }

  calibration_.gyroBias[0] = static_cast<float>(gyroSums[0] / sampleCount);
  calibration_.gyroBias[1] = static_cast<float>(gyroSums[1] / sampleCount);
  calibration_.gyroBias[2] = static_cast<float>(gyroSums[2] / sampleCount);
  calibration_.gyroBiasValid = true;
  invalidateReading();
  return true;
}

bool ImuSensor::startGyroCalibration(uint16_t sampleCount, uint16_t intervalMs) {
  if (!ready_ || sampleCount == 0 || intervalMs == 0 || gyroCalibrationActive_)
    return false;

  gyroCalibrationTarget_ = sampleCount;
  gyroCalibrationCount_ = 0;
  gyroCalibrationIntervalUs_ = static_cast<uint64_t>(intervalMs) * 1000ULL;
  gyroCalibrationNextSampleUs_ = static_cast<uint64_t>(esp_timer_get_time());
  for (double& sum : gyroCalibrationSums_) sum = 0.0;
  gyroCalibrationActive_ = true;
  return true;
}

ImuCalibrationEvent ImuSensor::serviceGyroCalibration() {
  if (!gyroCalibrationActive_) return ImuCalibrationEvent::NONE;
  const uint64_t nowUs = static_cast<uint64_t>(esp_timer_get_time());
  if (nowUs < gyroCalibrationNextSampleUs_) return ImuCalibrationEvent::NONE;

  float rawAxes[6] = {};
  if (!ready_ || !driver_.read(rawAxes) || !std::isfinite(rawAxes[3]) ||
      !std::isfinite(rawAxes[4]) || !std::isfinite(rawAxes[5])) {
    ready_ = false;
    gyroCalibrationActive_ = false;
    invalidateReading();
    reading_.timestampUs = static_cast<uint64_t>(esp_timer_get_time());
    reading_.timestampMs = millis();
    return ImuCalibrationEvent::FAILED;
  }

  gyroCalibrationSums_[0] += rawAxes[3];
  gyroCalibrationSums_[1] += rawAxes[4];
  gyroCalibrationSums_[2] += rawAxes[5];
  ++gyroCalibrationCount_;
  if (gyroCalibrationCount_ >= gyroCalibrationTarget_) {
    for (size_t axis = 0; axis < 3; ++axis) {
      calibration_.gyroBias[axis] = static_cast<float>(
          gyroCalibrationSums_[axis] / gyroCalibrationTarget_);
    }
    calibration_.gyroBiasValid = true;
    gyroCalibrationActive_ = false;
    invalidateReading();
    return ImuCalibrationEvent::COMPLETE;
  }

  // Schedule from the actual service time so a delayed task does not issue a
  // burst of catch-up reads against the same sensor register value.
  gyroCalibrationNextSampleUs_ = nowUs + gyroCalibrationIntervalUs_;
  return ImuCalibrationEvent::NONE;
}

void ImuSensor::clearCalibration() {
  gyroCalibrationActive_ = false;
  calibration_ = ImuCalibration{};
  invalidateReading();
}

void ImuSensor::setAccelerationBias(float x, float y, float z) {
  calibration_.accelerationBias[0] = x;
  calibration_.accelerationBias[1] = y;
  calibration_.accelerationBias[2] = z;
  invalidateReading();
}

void ImuSensor::setGyroBias(float x, float y, float z) {
  calibration_.gyroBias[0] = x;
  calibration_.gyroBias[1] = y;
  calibration_.gyroBias[2] = z;
  calibration_.gyroBiasValid = true;
  invalidateReading();
}

float ImuSensor::accelerationMagnitude() const {
  if (!reading_.valid) return NAN;
  return sqrtf(
      reading_.acceleration[0] * reading_.acceleration[0] +
      reading_.acceleration[1] * reading_.acceleration[1] +
      reading_.acceleration[2] * reading_.acceleration[2]);
}

float ImuSensor::gyroMagnitude() const {
  if (!reading_.valid) return NAN;
  return sqrtf(
      reading_.gyro[0] * reading_.gyro[0] +
      reading_.gyro[1] * reading_.gyro[1] +
      reading_.gyro[2] * reading_.gyro[2]);
}

void ImuSensor::invalidateReading() {
  reading_ = ImuReading{};
}

void ImuSensor::applyCalibration(const float rawAxes[6]) {
  reading_.acceleration[0] = rawAxes[0] - calibration_.accelerationBias[0];
  reading_.acceleration[1] = rawAxes[1] - calibration_.accelerationBias[1];
  reading_.acceleration[2] = rawAxes[2] - calibration_.accelerationBias[2];
  reading_.gyro[0] = rawAxes[3] - calibration_.gyroBias[0];
  reading_.gyro[1] = rawAxes[4] - calibration_.gyroBias[1];
  reading_.gyro[2] = rawAxes[5] - calibration_.gyroBias[2];
}
