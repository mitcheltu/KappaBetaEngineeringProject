#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <cmath>
#include <cstdint>

// Reuse the checked BMI270 transport used by the wearable firmware. The
// ImuSensor class below is the application-facing object; callers do not need
// to know about Bosch register access or the transport adapter.
#include "CheckedBMI270.h"

struct ImuReading {
  float acceleration[3] = {NAN, NAN, NAN}; // g
  float gyro[3] = {NAN, NAN, NAN};         // degrees per second
  uint32_t timestampMs = 0;
  bool valid = false;
};

struct ImuCalibration {
  float accelerationBias[3] = {0.0f, 0.0f, 0.0f}; // g
  float gyroBias[3] = {0.0f, 0.0f, 0.0f};         // degrees per second
  bool gyroBiasValid = false;
};

class ImuSensor {
 public:
  // Initializes the BMI270 and applies the repository's standard sensor
  // configuration: +/-8 g acceleration, +/-2000 dps gyro, 100 Hz.
  bool begin(TwoWire& bus, uint8_t address);

  // Reads one sample and applies the configured biases. The returned reading
  // remains available through reading() and the named accessors below.
  bool update();

  // Collects stationary gyro samples and stores their mean as gyro bias.
  // Keep the board motionless while this blocking operation runs. This does
  // not claim to calibrate accelerometer scale or multi-position bias.
  bool calibrateGyro(uint16_t sampleCount = 200, uint16_t intervalMs = 5);

  void clearCalibration();
  void setAccelerationBias(float x, float y, float z);
  void setGyroBias(float x, float y, float z);

  bool ready() const { return ready_; }
  bool hasReading() const { return reading_.valid; }
  const ImuReading& reading() const { return reading_; }
  const ImuCalibration& calibration() const { return calibration_; }

  // Named accessors keep the call site readable while still making the
  // object's current sensor state available without exposing driver internals.
  float accelerationX() const { return reading_.acceleration[0]; }
  float accelerationY() const { return reading_.acceleration[1]; }
  float accelerationZ() const { return reading_.acceleration[2]; }
  float gyroX() const { return reading_.gyro[0]; }
  float gyroY() const { return reading_.gyro[1]; }
  float gyroZ() const { return reading_.gyro[2]; }

  float accelerationMagnitude() const;
  float gyroMagnitude() const;

 private:
  void invalidateReading();
  void applyCalibration(const float rawAxes[6]);

  CheckedBMI270 driver_;
  ImuReading reading_;
  ImuCalibration calibration_;
  bool ready_ = false;
};
