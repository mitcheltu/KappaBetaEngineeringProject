#include "FirmwareShared.h"

void initializeImu() {
  imuValid = false;
  imuReady = imu.begin(Wire, Config::IMU_ADDRESS);
  if (!imuReady) ++sensorErrors;
}

void sampleImu() {
  if (!imuReady) return;
  imuValid = imu.read(imuAxes);
  if (imuValid) {
    for (size_t i = 0; i < 6; ++i) {
      const float scaled = imuAxes[i] * (i < 3 ? 1000.0f : 10.0f);
      imuValid = imuValid && std::isfinite(scaled) && scaled >= -32768 && scaled <= 32767;
    }
  }
  if (imuValid) imuMs = millis();
  else { ++sensorErrors; imuReady = false; }
  recordImuSample();
}
