#include "FirmwareShared.h"

void initializeImu() {
  static bool failureReported = false;
  logMessage(LogLevel::DEBUG, "IMU", "Initializing BMI270 at 0x%02X", unsigned(Config::IMU_ADDRESS));
  imuValid = false;
  imuReady = imu.begin(Wire, Config::IMU_ADDRESS);
  if (imuReady) {
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
  const bool wasReady = imuReady.load();
  const bool transferOk = wasReady && imu.update();
  imuValid = transferOk;
  ImuReading bufferedReading{};
  if (imuValid) {
    const ImuReading& reading = imu.reading();
    bufferedReading = reading;
    imuAxes[0] = reading.acceleration[0];
    imuAxes[1] = reading.acceleration[1];
    imuAxes[2] = reading.acceleration[2];
    imuAxes[3] = reading.gyro[0];
    imuAxes[4] = reading.gyro[1];
    imuAxes[5] = reading.gyro[2];
    imuMs = reading.timestampMs;
  } else {
    for (float& axis : imuAxes) axis = NAN;
    bufferedReading.timestampUs = static_cast<uint64_t>(esp_timer_get_time());
    bufferedReading.timestampMs = millis();
    bufferedReading.valid = false;
    if (wasReady) {
      logMessage(LogLevel::WARN, "IMU", "BMI270 sample read failed; readings invalid until reinitialization");
      ++sensorErrors;
      imuReady = false;
    }
  }

  // Keep one slot per 20 Hz attempt, including invalid reads, in the rolling
  // acquisition buffer. Processing copies its own stable window later.
  sensorBuffers.pushImu(bufferedReading);
  if (sensorBuffers.imuCount() >= ImuRisk::IMU_WINDOW_SAMPLES &&
      processingTaskHandle) {
    xTaskNotify(processingTaskHandle, 1, eSetValueWithOverwrite);
  }

  recordImuSample(bufferedReading.timestampUs);
}

bool calibrateImuGyro() {
  if (!imuReady.load()) return false;
  bool expected = false;
  return gyroCalibrationRequested.compare_exchange_strong(expected, true);
}

void clearImuCalibration() {
  gyroCalibrationClearRequested.store(true);
}

void processingTask(void*) {
  ImuReading snapshot[ImuRisk::IMU_WINDOW_SAMPLES]{};
  uint32_t notification = 0;
  for (;;) {
    if (xTaskNotifyWait(0, UINT32_MAX, &notification, portMAX_DELAY) != pdTRUE)
      continue;

    const uint64_t targetUs = static_cast<uint64_t>(esp_timer_get_time());
    const std::size_t copied = sensorBuffers.copyLatestImu(
        targetUs, snapshot, ImuRisk::IMU_WINDOW_SAMPLES);
    if (copied != ImuRisk::IMU_WINDOW_SAMPLES) continue;

    imuWindow.clear();
    for (std::size_t i = 0; i < copied; ++i) imuWindow.push(snapshot[i]);
    if (!imuWindow.allValid()) continue;

    const uint8_t classification = imuRiskEvaluator.evaluate(
        imuWindow.tensor(), imuWindow.startTimestampUs(),
        imuWindow.endTimestampUs());
    const ImuReading& latest = snapshot[copied - 1];
    const float accelerationMagnitude = sqrtf(
        latest.acceleration[0] * latest.acceleration[0] +
        latest.acceleration[1] * latest.acceleration[1] +
        latest.acceleration[2] * latest.acceleration[2]);
    const float gyroMagnitude = sqrtf(
        latest.gyro[0] * latest.gyro[0] +
        latest.gyro[1] * latest.gyro[1] +
        latest.gyro[2] * latest.gyro[2]);
    logMessage(LogLevel::DEBUG, "IMU", "window=%u/%u interval=%llu..%llu us risk=%u score=%.3f accel=%.3fg gyro=%.3fdps",
        static_cast<unsigned>(copied), static_cast<unsigned>(ImuRisk::IMU_WINDOW_SAMPLES),
        static_cast<unsigned long long>(imuRiskEvaluator.result().inputStartUs),
        static_cast<unsigned long long>(imuRiskEvaluator.result().inputEndUs),
        static_cast<unsigned>(classification), imuRiskEvaluator.lastScore(),
        accelerationMagnitude, gyroMagnitude);
  }
}
