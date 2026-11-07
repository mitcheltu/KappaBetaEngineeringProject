#include "FirmwareShared.h"

void initializeImu() {
  static bool failureReported = false;
  logMessage(LogLevel::DEBUG, "IMU", "Initializing BMI270 at 0x%02X", unsigned(Config::IMU_ADDRESS));
  imuValid = false;
  imuReady = imu.begin(Wire, Config::IMU_ADDRESS);
  if (imuReady) {
    logMessage(LogLevel::INFO, "IMU", "BMI270 ready: accel +/-8g, gyro +/-2000dps, 100Hz sensor rate");
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
  if (!imuReady) return;
  imuValid = imu.read(imuAxes);
  const bool transferOk = imuValid;
  if (imuValid) {
    for (size_t i = 0; i < 6; ++i) {
      const float scaled = imuAxes[i] * (i < 3 ? 1000.0f : 10.0f);
      imuValid = imuValid && std::isfinite(scaled) && scaled >= -32768 && scaled <= 32767;
    }
  }
  if (imuValid) imuMs = millis();
  else {
    logMessage(LogLevel::WARN, "IMU", "%s; readings invalid until reinitialization",
               transferOk ? "BMI270 sample nonfinite or outside telemetry range" : "BMI270 sample read failed");
    ++sensorErrors; imuReady = false;
  }
  recordImuSample();
}
