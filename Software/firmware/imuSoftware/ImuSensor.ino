#include "ImuSensor.h"

#include <cmath>

bool ImuSensor::begin(TwoWire& bus, uint8_t address) {
  invalidateReading();
  ready_ = driver_.begin(bus, address);
  return ready_;
}

bool ImuSensor::update() {
  float rawAxes[6] = {};
  if (!ready_ || !driver_.read(rawAxes)) {
    ready_ = false;
    invalidateReading();
    return false;
  }

  applyCalibration(rawAxes);
  reading_.timestampMs = millis();
  reading_.valid = true;
  return true;
}

bool ImuSensor::calibrateGyro(uint16_t sampleCount, uint16_t intervalMs) {
  if (!ready_ || sampleCount == 0) return false;

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

void ImuSensor::clearCalibration() {
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
