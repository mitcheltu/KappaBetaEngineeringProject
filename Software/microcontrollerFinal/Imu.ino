#include "FirmwareShared.h"

void initializeImu() {
  static bool failureReported = false;
  logMessage(LogLevel::DEBUG, "IMU", "Initializing BMI270 at 0x%02X", unsigned(Config::IMU_ADDRESS));
  imuValid = false;
  imuReady = imu.begin(Wire, Config::IMU_ADDRESS);
  if (imuReady) {
    imuWindow.clear();
    logMessage(LogLevel::INFO, "IMU", "BMI270 ready: accel +/-8g, gyro +/-2000dps, 100Hz sensor rate; firmware samples at 20Hz");
    failureReported = false;
  } else {
    ++sensorErrors;
    const LogLevel level = failureReported ? LogLevel::DEBUG : LogLevel::WARN;
    uint8_t chipId = 0;
    if (!readRegisters(Config::IMU_ADDRESS, 0x00, &chipId, 1)) {
      logMessage(level, "IMU", "BMI270 init failed at 0x%02X: ID read failed; check power, address and I2C wiring", unsigned(Config::IMU_ADDRESS));
    } else if (chipId != 0x24) {
      logMessage(level, "IMU", "BMI270 init failed: reg 0x00=0x%02X, expected ID 0x24; MPU6050 is not compatible", unsigned(chipId));
    } else {
      logMessage(level, "IMU", "BMI270 ID OK but initialization/configuration failed; retry pending");
    }
    failureReported = true;
  }
}

void sampleImu() {
  const bool transferOk = imuReady && imu.update();
  imuValid = transferOk;
  if (imuValid) {
    const ImuReading& reading = imu.reading();
    imuAxes[0] = reading.acceleration[0];
    imuAxes[1] = reading.acceleration[1];
    imuAxes[2] = reading.acceleration[2];
    imuAxes[3] = reading.gyro[0];
    imuAxes[4] = reading.gyro[1];
    imuAxes[5] = reading.gyro[2];
    imuMs = reading.timestampMs;
  } else {
    for (float& axis : imuAxes) axis = NAN;
    logMessage(LogLevel::WARN, "IMU", "%s; readings invalid until reinitialization",
               transferOk ? "BMI270 sample nonfinite or outside telemetry range" : "BMI270 sample read failed");
    ++sensorErrors;
    imuReady = false;
  }

  // Push one time slot every 50 ms. A failed transfer occupies an invalid
  // slot, preventing a 60-valid-sample window from spanning more than 3 s.
  imuWindow.push(imu.reading());
  if (imuWindow.allValid()) {
    const uint8_t risk = imuRiskEvaluator.evaluate(imuWindow.tensor());
    logMessage(LogLevel::DEBUG, "IMU", "window=%u/%u risk=%u score=%.3f accel=%.3fg gyro=%.3fdps",
               unsigned(imuWindow.sampleCount()), unsigned(ImuRisk::IMU_WINDOW_SAMPLES),
               unsigned(risk), imuRiskEvaluator.lastScore(),
               imu.accelerationMagnitude(), imu.gyroMagnitude());
  }

  recordImuSample();
}

bool calibrateImuGyro() {
  if (!imuReady) return false;
  const bool calibrated = imu.calibrateGyro();
  imuValid = false;
  imuWindow.clear();
  for (float& axis : imuAxes) axis = NAN;
  imuReady = calibrated;
  return calibrated;
}

void clearImuCalibration() {
  imu.clearCalibration();
  imuValid = false;
  imuWindow.clear();
  for (float& axis : imuAxes) axis = NAN;
}
