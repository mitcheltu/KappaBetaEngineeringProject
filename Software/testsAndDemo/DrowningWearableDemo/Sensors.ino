#include "FirmwareShared.h"
#include <Wire.h>
#include <SparkFun_BMI270_Arduino_Library.h>
#include <MAX30105.h>
#include <spo2_algorithm.h>
#include <MS5837.h>

#ifndef MS5837_H_BLUEROBOTICS
#error "Install BlueRobotics MS5837 Library 1.1.1."
#endif

namespace DemoSensors {
static_assert(Config::PPG_SAMPLE_HZ == 100 && Config::PPG_AVERAGING == 4,
              "This stock-library demo feeds the Maxim algorithm at 25 Hz.");
BMI270 imu;
MAX30105 ppg;
MS5837 depth;
uint32_t lastImuMs = 0, lastDepthMs = 0, lastRetryMs = 0;
uint32_t lastPpgServiceMs = 0;
uint32_t redWindow[100] = {}, irWindow[100] = {};
size_t windowCount = 0;

// Address ACK only: all sensor registers/configuration/data use stock libraries.
bool present(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

void resetOpticalWindow() {
  windowCount = 0;
  sensors.hrValid = sensors.spo2Valid = false;
}

void discardOpticalQueue() {
  while (ppg.available()) ppg.nextSample();
  ppg.clearFIFO();
  resetOpticalWindow();
  sensors.rawPpgValid = false;
  sensors.ppgMs = lastPpgServiceMs = millis();
}

void initImu() {
  sensors.imuReady = sensors.imuValid = false;
  for (float& axis : sensors.imuAxes) axis = NAN;
  bool ok = present(Config::IMU_ADDRESS) &&
            imu.beginI2C(Config::IMU_ADDRESS, Wire) == BMI2_OK;
  bmi2_sens_config config[2] = {};
  config[0].type = BMI2_ACCEL;
  config[1].type = BMI2_GYRO;
  if (ok) ok = imu.getConfigs(config, 2) == BMI2_OK;
  if (ok) {
    config[0].cfg.acc.odr = BMI2_ACC_ODR_100HZ;
    config[0].cfg.acc.range = BMI2_ACC_RANGE_8G;
    config[1].cfg.gyr.odr = BMI2_GYR_ODR_100HZ;
    config[1].cfg.gyr.range = BMI2_GYR_RANGE_2000;
    ok = imu.setConfigs(config, 2) == BMI2_OK &&
         imu.getConfigs(config, 2) == BMI2_OK &&
         config[0].cfg.acc.odr == BMI2_ACC_ODR_100HZ &&
         config[0].cfg.acc.range == BMI2_ACC_RANGE_8G &&
         config[1].cfg.gyr.odr == BMI2_GYR_ODR_100HZ &&
         config[1].cfg.gyr.range == BMI2_GYR_RANGE_2000;
  }
  sensors.imuReady = ok;
  sensors.imuMs = millis();
  if (!ok) ++sensors.sensorErrors;
  logMessage(ok ? LogLevel::INFO : LogLevel::WARN, "IMU",
      ok ? "BMI270 ready: 100 Hz hardware, 20 Hz host target, +/-8g and +/-2000dps" :
           "BMI270 unavailable/configuration failed; retry in 5 s");
}

void initDepth() {
  sensors.depthReady = sensors.pressureValid = false;
  sensors.pressurePa = sensors.temperatureC = sensors.surfacePa = NAN;
  depth.setModel(MS5837::MS5837_30BA);
  sensors.depthReady = present(Config::DEPTH_ADDRESS) && depth.init();
  if (!sensors.depthReady) ++sensors.sensorErrors;
  logMessage(sensors.depthReady ? LogLevel::INFO : LogLevel::WARN, "DEPTH",
      sensors.depthReady ? "MS5837-30BA ready: 5 Hz target; send z at surface to zero" :
                           "MS5837 unavailable/PROM check failed; retry in 5 s");
}

void initPpg() {
  sensors.ppgReady = sensors.rawPpgValid = false;
  resetOpticalWindow();
  if (present(Config::PPG_ADDRESS) && ppg.begin(Wire, Config::I2C_HZ, Config::PPG_ADDRESS)) {
    ppg.setup(60, Config::PPG_AVERAGING, 2, Config::PPG_SAMPLE_HZ, 411, 4096);
    ppg.disableFIFORollover();
    sensors.ppgReady = present(Config::PPG_ADDRESS) && ppg.readPartID() == 0x15;
    if (sensors.ppgReady) discardOpticalQueue();
  }
  if (!sensors.ppgReady) ++sensors.sensorErrors;
  logMessage(sensors.ppgReady ? LogLevel::INFO : LogLevel::WARN, "PPG",
      sensors.ppgReady ? "MAX30102 ready: 25 averaged pairs/s; first HR/SpO2 window needs 4 s of good signal" :
                         "MAX30102 unavailable; retry in 5 s");
}

void acceptOpticalSample(uint32_t red, uint32_t ir) {
  sensors.red = red;
  sensors.ir = ir;
  sensors.ppgMs = millis();
  sensors.rawPpgValid = true;
  if (ir < Config::PPG_MIN_IR || red == 0 ||
      ir >= Config::PPG_MAX_ADC || red >= Config::PPG_MAX_ADC) {
    resetOpticalWindow();
    return;
  }
  redWindow[windowCount] = red;
  irWindow[windowCount++] = ir;
  if (windowCount < 100) return;
  int8_t validHr = 0, validSpo2 = 0;
  maxim_heart_rate_and_oxygen_saturation(irWindow, 100, redWindow,
      &sensors.oxygenSaturation, &validSpo2, &sensors.heartRate, &validHr);
  sensors.hrValid = validHr != 0 && sensors.heartRate > 0 && sensors.heartRate < 6553;
  sensors.spo2Valid = validSpo2 != 0 && sensors.oxygenSaturation > 0 && sensors.oxygenSaturation <= 100;
  sensors.estimateMs = millis();
  memmove(redWindow, redWindow + 25, 75 * sizeof(uint32_t));
  memmove(irWindow, irWindow + 25, 75 * sizeof(uint32_t));
  windowCount = 75;
}

void servicePpg() {
  if (!sensors.ppgReady) return;
  const uint32_t now = millis();
  const uint32_t gap = now - lastPpgServiceMs;
  if (gap < 5) return; // Poll often enough for the stock four-slot software queue.
  lastPpgServiceMs = now;
  if (!present(Config::PPG_ADDRESS)) {
    sensors.ppgReady = sensors.rawPpgValid = false;
    resetOpticalWindow();
    ++sensors.sensorErrors;
    logMessage(LogLevel::WARN, "PPG", "MAX30102 lost ACK; readings invalid until retry");
    return;
  }
  // check() copies every pending hardware sample into a four-slot ring. Four
  // or more arrivals wrap/overwrite that ring; never estimate through a gap.
  const uint16_t added = ppg.check();
  if (gap >= Config::PPG_SERVICE_GAP_MS || added >= STORAGE_SIZE) {
    ++sensors.ppgOverflows;
    discardOpticalQueue();
    logMessage(LogLevel::WARN, "PPG", "Window reset: service gap=%lums batch=%u; stock queue has 4 slots",
        static_cast<unsigned long>(gap), unsigned(added));
    return;
  }
  while (ppg.available()) {
    // In stock 1.1.2 check() advances head before writing; advance tail before
    // getters to consume the newly written pair rather than the previous slot.
    ppg.nextSample();
    acceptOpticalSample(ppg.getFIFORed(), ppg.getFIFOIR());
  }
  if (millis() - sensors.ppgMs >= 1000) {
    sensors.ppgReady = sensors.rawPpgValid = false;
    resetOpticalWindow();
    ++sensors.sensorErrors;
    logMessage(LogLevel::WARN, "PPG", "No new optical samples for 1 s; retry pending");
  }
}

void sampleImu() {
  if (!sensors.imuReady) return;
  uint8_t status = 0;
  bool ok = present(Config::IMU_ADDRESS) && imu.getStatus(&status) == BMI2_OK;
  if (ok && (status & (BMI2_DRDY_ACC | BMI2_DRDY_GYR)) != (BMI2_DRDY_ACC | BMI2_DRDY_GYR)) {
    if (millis() - sensors.imuMs < 1000) return;
    ok = false;
  }
  if (ok) ok = imu.getSensorData() == BMI2_OK;
  if (ok) {
    const float axes[6] = {imu.data.accelX, imu.data.accelY, imu.data.accelZ,
                          imu.data.gyroX, imu.data.gyroY, imu.data.gyroZ};
    for (size_t i = 0; i < 6; ++i) {
      const float scaled = axes[i] * (i < 3 ? 1000.0f : 10.0f);
      ok = ok && std::isfinite(scaled) && scaled >= -32768 && scaled <= 32767;
      sensors.imuAxes[i] = axes[i];
    }
  }
  sensors.imuValid = ok;
  if (ok) sensors.imuMs = millis();
  else {
    sensors.imuReady = false;
    for (float& axis : sensors.imuAxes) axis = NAN;
    ++sensors.sensorErrors;
    logMessage(LogLevel::WARN, "IMU", "BMI270 read/data-ready/range check failed; retry pending");
  }
}

bool sampleDepth() {
  sensors.pressureValid = false;
  if (!sensors.depthReady) return false;
  bool ok = present(Config::DEPTH_ADDRESS);
  if (ok) {
    depth.read(); // Original BlueRobotics library: approximately 40 ms blocking.
    sensors.pressurePa = depth.pressure(MS5837::Pa);
    sensors.temperatureC = depth.temperature();
    ok = present(Config::DEPTH_ADDRESS) &&
         std::isfinite(sensors.pressurePa) && sensors.pressurePa > 0 && sensors.pressurePa <= 3000000 &&
         std::isfinite(sensors.temperatureC) && sensors.temperatureC >= -20 && sensors.temperatureC <= 85;
  }
  sensors.pressureValid = ok;
  if (ok) sensors.pressureMs = millis();
  else {
    sensors.depthReady = false;
    sensors.pressurePa = sensors.temperatureC = sensors.surfacePa = NAN;
    ++sensors.sensorErrors;
    logMessage(LogLevel::WARN, "DEPTH", "MS5837 ACK/range check failed; baseline cleared, retry pending");
  }
  return ok;
}
} // namespace DemoSensors

void initializeSensors() {
  sensors.busReady = Wire.begin(Config::I2C_SDA, Config::I2C_SCL, Config::I2C_HZ);
  Wire.setTimeOut(20);
  if (sensors.busReady) {
    DemoSensors::initImu();
    DemoSensors::initDepth();
    DemoSensors::initPpg(); // Optical sampling starts after blocking initialization.
  } else {
    ++sensors.sensorErrors;
    logMessage(LogLevel::WARN, "I2C", "Shared bus initialization failed; retry pending");
  }
  DemoSensors::lastRetryMs = millis();
}

void serviceSensors(uint32_t now) {
  using namespace DemoSensors;
  servicePpg();
  if (zeroRequested.exchange(false)) {
    sensors.surfacePa = NAN;
    lastDepthMs = millis();
    if (sampleDepth()) {
      sensors.surfacePa = sensors.pressurePa;
      logReply("DEPTH", "Surface zero=%.1fPa; keep sensor at water surface while zeroing", sensors.surfacePa);
    } else logReply("DEPTH", "Surface zero failed: no valid pressure reading");
    servicePpg();
  }
  now = millis();
  if (now - lastImuMs >= Config::IMU_PERIOD_MS) { lastImuMs = now; sampleImu(); }
  if (now - lastDepthMs >= Config::DEPTH_PERIOD_MS) {
    lastDepthMs = now;
    servicePpg(); sampleDepth(); servicePpg();
  }
  now = millis();
  if (now - lastRetryMs >= Config::SENSOR_RETRY_MS) {
    lastRetryMs = now;
    if (!sensors.busReady) {
      sensors.busReady = Wire.begin(Config::I2C_SDA, Config::I2C_SCL, Config::I2C_HZ);
      Wire.setTimeOut(20);
    }
    if (sensors.busReady) {
      if (!sensors.imuReady) { initImu(); servicePpg(); }
      if (!sensors.depthReady) { initDepth(); servicePpg(); }
      if (!sensors.ppgReady) initPpg();
    }
  }
  servicePpg();
}
