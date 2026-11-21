#include "FirmwareShared.h"
#include <Wire.h>
#include <SparkFun_BMI270_Arduino_Library.h>
#include <MAX30105.h>
#include <spo2_algorithm.h>
#include <MS5837.h>
#include <esp_timer.h>
#include "PpgDiagnostics.h"
#include "PpgFifo.h"
#include "DoubleTap.h"

std::atomic<bool> tapCancelRequested{false};
std::atomic<uint32_t> doubleTapCount{0};

#ifndef MS5837_H_BLUEROBOTICS
#error "Install BlueRobotics MS5837 Library 1.1.1."
#endif

namespace DemoSensors {
static_assert(Config::PPG_SAMPLE_HZ == 100 && Config::PPG_AVERAGING == 4,
              "This stock-library demo feeds the Maxim algorithm at 25 Hz.");
static_assert(STORAGE_SIZE == 4, "Review PpgFifo's software queue limit after a library upgrade.");
BMI270 imu;
MAX30105 ppg;
MS5837 depth;
uint32_t lastImuMs = 0, lastDepthMs = 0, lastRetryMs = 0;
uint32_t lastPpgServiceMs = 0;
uint64_t lastPpgSampleUs = 0;
uint32_t redWindow[100] = {}, irWindow[100] = {};
size_t windowCount = 0;
bool ppgLibraryBound = false;
uint32_t ppgSamples = 0, ppgAttempts = 0;
// Double tap: every 100 Hz accelerometer sample from the BMI270 FIFO (12-byte acc+gyr
// frames; the 2 KiB FIFO holds ~1.7 s, far longer than the 40 ms depth read stalls).
DoubleTap doubleTap;
bool tapFifoReady = false;
BMI270_SensorData fifoFrames[176];

// Address ACK only: all sensor registers/configuration/data use stock libraries.
bool present(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

void logPpgConfiguration(LogLevel level, const PpgDiagnostics::Configuration& c) {
  logMessage(level, "PPG_REG", "part=%02X rev=%02X mode=%02X spo2=%02X fifo=%02X LED=%u/%u config_ok=%d",
      unsigned(c.part), unsigned(c.revision), unsigned(c.mode), unsigned(c.spo2),
      unsigned(c.fifo), unsigned(c.redLed), unsigned(c.irLed), c.matches());
}

void logPpgRegisters(LogLevel level) {
  if (!sensors.busReady || !ppgLibraryBound || !present(Config::PPG_ADDRESS)) {
    logMessage(level, "PPG_REG", "No initialized I2C connection to 0x57; attempts=%lu samples=%lu",
        static_cast<unsigned long>(ppgAttempts), static_cast<unsigned long>(ppgSamples));
    return;
  }
  logPpgConfiguration(level, PpgDiagnostics::readConfiguration(ppg));
  const uint8_t write = ppg.getWritePointer(), read = ppg.getReadPointer();
  const uint8_t overflow = ppg.readRegister8(Config::PPG_ADDRESS, 0x05);
  // Status reads clear interrupt flags, but never consume FIFO samples. This
  // sketch polls the FIFO and does not use interrupt-driven acquisition.
  const uint8_t status = ppg.getINT1();
  logMessage(level, "PPG_FIFO", "WR=%u RD=%u OVF=%u status=%02X attempts=%lu samples=%lu window=%u ready=%d raw=%d",
      unsigned(write), unsigned(read), unsigned(overflow), unsigned(status),
      static_cast<unsigned long>(ppgAttempts), static_cast<unsigned long>(ppgSamples),
      unsigned(windowCount), sensors.ppgReady, sensors.rawPpgValid);
}

void resetOpticalWindow() {
  windowCount = 0;
  sensors.hrValid = sensors.spo2Valid = false;
}

void discardOpticalQueue() {
  PpgFifo::discard(ppg);
  resetOpticalWindow();
  sensors.rawPpgValid = false;
  sensors.ppgMs = lastPpgServiceMs = millis();
  recordPpgGap();
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
  // Double-tap input: the FIFO keeps every 100 Hz sample between loop passes. The
  // 20 Hz data-register reads above (telemetry/history) are unaffected.
  tapFifoReady = false;
  doubleTap.reset();
  if (ok && Config::DOUBLE_TAP_CANCEL) {
    BMI270_FIFOConfig fifo{};
    fifo.flags = BMI2_FIFO_ACC_EN | BMI2_FIFO_GYR_EN;
    fifo.watermark = 64;
    fifo.accelDownSample = BMI2_FIFO_DOWN_SAMPLE_1;
    fifo.gyroDownSample = BMI2_FIFO_DOWN_SAMPLE_1;
    fifo.accelFilter = BMI2_ENABLE;
    fifo.gyroFilter = BMI2_ENABLE;
    fifo.selfWakeUp = BMI2_ENABLE;
    tapFifoReady = imu.setFIFOConfig(fifo) == BMI2_OK && imu.flushFIFO() == BMI2_OK;
    logMessage(tapFifoReady ? LogLevel::INFO : LogLevel::WARN, "TAP",
        tapFifoReady ? "Double tap = Cancel (100 Hz FIFO, %.1f g jolt, %lu-%lu ms apart)" :
                       "BMI270 FIFO setup failed; double tap disabled until the IMU reinitializes",
        DoubleTap::TAP_G, static_cast<unsigned long>(DoubleTap::DT_MIN), static_cast<unsigned long>(DoubleTap::DT_MAX));
  }
}

// Feed every queued 100 Hz accelerometer sample to the detectioncode.md detector.
void serviceTaps() {
  if (!tapFifoReady || !sensors.imuReady) return;
  uint16_t frames = sizeof(fifoFrames) / sizeof(fifoFrames[0]);
  if (imu.getFIFOData(fifoFrames, &frames) != BMI2_OK) {
    tapFifoReady = false;
    logMessage(LogLevel::WARN, "TAP", "BMI270 FIFO read failed; double tap disabled until the IMU reinitializes");
    return;
  }
  const uint32_t now = millis();
  for (uint16_t i = 0; i < frames; ++i) {
    // Samples are 10 ms apart; the newest frame is "now".
    const uint32_t t = now - uint32_t(frames - 1 - i) * 10u;
    if (doubleTap.update(fifoFrames[i].accelX, fifoFrames[i].accelY, fifoFrames[i].accelZ, t)) {
      doubleTapCount.fetch_add(1);
      tapCancelRequested.store(true);
    }
  }
}

void initDepth() {
  const bool hadBaseline = std::isfinite(sensors.surfacePa);
  sensors.depthReady = sensors.pressureValid = false;
  sensors.pressurePa = sensors.temperatureC = sensors.surfacePa = NAN;
  if (hadBaseline) recordSurfaceZero();
  depth.setModel(MS5837::MS5837_30BA);
  sensors.depthReady = present(Config::DEPTH_ADDRESS) && depth.init();
  if (!sensors.depthReady) ++sensors.sensorErrors;
  logMessage(sensors.depthReady ? LogLevel::INFO : LogLevel::WARN, "DEPTH",
      sensors.depthReady ? "MS5837-30BA ready: 5 Hz target; send z at surface to zero" :
                           "MS5837 unavailable/PROM check failed; retry in 5 s");
}

void initPpg() {
  ++ppgAttempts;
  sensors.ppgReady = sensors.rawPpgValid = false;
  resetOpticalWindow();
  if (present(Config::PPG_ADDRESS)) {
    ppgLibraryBound = true; // begin() binds Wire even if its part-ID check fails.
    if (ppg.begin(Wire, Config::I2C_HZ, Config::PPG_ADDRESS)) {
      ppg.setup(Config::PPG_LED_AMPLITUDE, Config::PPG_AVERAGING, 2, Config::PPG_SAMPLE_HZ, 411, 4096);
      // Retain the stock setup's rollover behavior. Equal read/write pointers
      // with rollover disabled can strand a full FIFO behind check() == 0.
      ppg.enableFIFORollover();
      const auto configuration = PpgDiagnostics::readConfiguration(ppg);
      sensors.ppgReady = configuration.matches();
      logPpgConfiguration(sensors.ppgReady ? LogLevel::DEBUG : LogLevel::WARN, configuration);
      if (sensors.ppgReady) {
        discardOpticalQueue();
        ppg.getINT1(); // Clear startup flags before diagnosing later stalls.
      }
    }
  }
  if (!sensors.ppgReady) ++sensors.sensorErrors;
  logMessage(sensors.ppgReady ? LogLevel::INFO : LogLevel::WARN, "PPG",
      sensors.ppgReady ? "MAX30102 ready: 25 averaged pairs/s; first HR/SpO2 window needs 4 s of good signal" :
                         "MAX30102 ACK/part-ID/config check failed; retry in 5 s");
  if (!sensors.ppgReady) logPpgRegisters(LogLevel::WARN);
}

void acceptOpticalSample(uint32_t red, uint32_t ir) {
  ++ppgSamples;
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
    recordPpgGap();
    logMessage(LogLevel::WARN, "PPG", "MAX30102 lost ACK; readings invalid until retry");
    return;
  }
  // Detect hardware overflow before stock check() can hide it. Also reject
  // batches that wrap its four-slot software ring; never estimate across loss.
  const auto fifo = PpgFifo::poll(ppg, gap);
  if (fifo.discontinuity()) {
    ++sensors.ppgOverflows;
    discardOpticalQueue();
    logMessage(LogLevel::WARN, "PPG", "FIFO recovery: lost=%u gap=%lums batch=%u; discarded window and cleared queues",
        unsigned(fifo.overflow), static_cast<unsigned long>(gap), unsigned(fifo.added));
    return;
  }
  const uint64_t readTimeUs = esp_timer_get_time();
  while (ppg.available()) {
    const uint64_t ageUs = uint64_t(ppg.available() - 1) * Config::PPG_PERIOD_US;
    uint64_t sampleUs = readTimeUs > ageUs ? readTimeUs - ageUs : 0;
    // FIFO timestamps are estimated, but must not reverse within the PPG stream.
    if (sampleUs <= lastPpgSampleUs) sampleUs = lastPpgSampleUs + 1;
    lastPpgSampleUs = sampleUs;
    // In stock 1.1.2 check() advances head before writing; advance tail before
    // getters to consume the newly written pair rather than the previous slot.
    ppg.nextSample();
    acceptOpticalSample(ppg.getFIFORed(), ppg.getFIFOIR());
    recordPpgSample(sampleUs);
  }
  if (millis() - sensors.ppgMs >= 1000) {
    // Capture the configuration and hardware FIFO before retry reconfigures it.
    logPpgRegisters(LogLevel::WARN);
    sensors.ppgReady = sensors.rawPpgValid = false;
    resetOpticalWindow();
    ++sensors.sensorErrors;
    recordPpgGap();
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
  recordImuSample();
}

bool sampleDepth() {
  const bool hadBaseline = std::isfinite(sensors.surfacePa);
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
    if (hadBaseline) recordSurfaceZero();
    ++sensors.sensorErrors;
    logMessage(LogLevel::WARN, "DEPTH", "MS5837 ACK/range check failed; baseline cleared, retry pending");
  }
  return ok;
}
} // namespace DemoSensors

void logPpgDiagnostics() {
  // Serial commands run in the same loop as sensor I2C access.
  DemoSensors::logPpgRegisters(LogLevel::INFO); // Enable log info/debug to see the dump.
}

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
    recordSurfaceZero();
    recordDepthSample();
    servicePpg();
  }
  now = millis();
  if (now - lastImuMs >= Config::IMU_PERIOD_MS) {
    lastImuMs = now; sampleImu(); serviceTaps();
  }
  if (now - lastDepthMs >= Config::DEPTH_PERIOD_MS) {
    lastDepthMs = now;
    servicePpg(); sampleDepth(); recordDepthSample(); servicePpg(); serviceTaps();
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
