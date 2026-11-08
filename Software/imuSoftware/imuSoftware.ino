#include <Arduino.h>
#include <Wire.h>
#include <cstring>

#include "ImuRiskEvaluator.h"
#include "ImuSensor.h"

namespace Config {
constexpr int I2C_SDA = 8;
constexpr int I2C_SCL = 9;
constexpr uint8_t IMU_ADDRESS = 0x68;
constexpr uint32_t I2C_HZ = 400000;
constexpr uint32_t IMU_PERIOD_MS = 10; // 100 Hz, matching BMI270 configuration.
}

ImuSensor imu;
ImuRisk::Evaluator imuRiskEvaluator;
ImuRisk::ImuTensor imuWindow = {};
size_t imuWindowSamples = 0;
uint32_t lastImuSampleMs = 0;

void appendCurrentImuSample() {
  if (!imu.hasReading()) return;

  if (imuWindowSamples == ImuRisk::IMU_WINDOW_SAMPLES) {
    for (size_t sample = 1; sample < ImuRisk::IMU_WINDOW_SAMPLES; ++sample) {
      for (size_t channel = 0; channel < ImuRisk::IMU_CHANNELS; ++channel) {
        imuWindow[sample - 1][channel] = imuWindow[sample][channel];
      }
    }
    --imuWindowSamples;
  }

  float* destination = imuWindow[imuWindowSamples++];
  destination[0] = imu.accelerationX();
  destination[1] = imu.accelerationY();
  destination[2] = imu.accelerationZ();
  destination[3] = imu.gyroX();
  destination[4] = imu.gyroY();
  destination[5] = imu.gyroZ();

  if (imuWindowSamples == ImuRisk::IMU_WINDOW_SAMPLES) {
    const uint8_t risk = imuRiskEvaluator.evaluate(imuWindow);
    Serial.printf("IMU window: risk=%u score=%.3f accel=%.3fg gyro=%.3fdps\n",
                  unsigned(risk), imuRiskEvaluator.lastScore(),
                  imu.accelerationMagnitude(), imu.gyroMagnitude());
  }
}

void serviceSerial() {
  static char command[24] = {};
  static size_t length = 0;

  while (Serial.available()) {
    const char character = static_cast<char>(Serial.read());
    if (character == '\r') continue;
    if (character != '\n') {
      if (length + 1 < sizeof(command)) command[length++] = character;
      continue;
    }

    command[length] = '\0';
    if (strcmp(command, "calibrate gyro") == 0) {
      Serial.println("Keep the IMU motionless: calibrating gyro bias...");
      const bool calibrated = imu.calibrateGyro();
      Serial.println(calibrated ? "Gyro calibration complete." :
                                   "Gyro calibration failed; IMU unavailable.");
      imuWindowSamples = 0;
    } else if (strcmp(command, "clear calibration") == 0) {
      imu.clearCalibration();
      imuWindowSamples = 0;
      Serial.println("IMU calibration cleared.");
    } else if (length != 0) {
      Serial.println("Commands: calibrate gyro | clear calibration");
    }
    length = 0;
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin(Config::I2C_SDA, Config::I2C_SCL, Config::I2C_HZ);
  Wire.setTimeOut(20);

  if (imu.begin(Wire, Config::IMU_ADDRESS)) {
    Serial.println("IMU ready: use imu.update() and named accessors for readings.");
  } else {
    Serial.println("IMU initialization failed.");
  }
}

void loop() {
  serviceSerial();

  const uint32_t now = millis();
  if (now - lastImuSampleMs >= Config::IMU_PERIOD_MS) {
    lastImuSampleMs = now;
    if (imu.update()) appendCurrentImuSample();
  }
}
