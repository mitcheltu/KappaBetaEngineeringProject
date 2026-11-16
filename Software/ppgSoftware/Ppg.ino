#include "FirmwareShared.h"

namespace {
MAX30105 sensor;
uint64_t lastSampleUs = 0;
uint32_t lastServiceMs = 0, lastDataMs = 0;

void markSensorGap() {
  sampleGeneration.fetch_add(1);
  sensorGaps.fetch_add(1);
  lastSampleUs = 0;
}
}

bool readPpgRegisters(uint8_t reg, uint8_t* data, size_t count) {
  Wire.beginTransmission(Config::PPG_ADDRESS);
  if (Wire.write(reg) != 1 || Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(Config::PPG_ADDRESS, count, true) != count) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    const int v = Wire.read();
    if (v < 0) return false;
    data[i] = static_cast<uint8_t>(v);
  }
  return true;
}

bool writePpgRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(Config::PPG_ADDRESS);
  const bool queued = Wire.write(reg) == 1 && Wire.write(value) == 1;
  return Wire.endTransmission() == 0 && queued;
}

bool clearPpgFifo() {
  return writePpgRegister(0x04, 0) && writePpgRegister(0x05, 0) && writePpgRegister(0x06, 0);
}

bool initializePpg() {
  markSensorGap();
  if (!sensor.begin(Wire, Config::I2C_HZ, Config::PPG_ADDRESS)) return false;
  sensor.setup(Config::LED_CURRENT, 1, 2, Config::PPG_SAMPLE_HZ, 411, 4096);
  sensor.disableFIFORollover();
  uint8_t mode = 0, spo2 = 0, fifo = 0, leds[2] = {};
  const bool valid = readPpgRegisters(0x09, &mode, 1) &&
      readPpgRegisters(0x0a, &spo2, 1) && readPpgRegisters(0x08, &fifo, 1) &&
      readPpgRegisters(0x0c, leds, 2) && mode == 3 && (spo2 & 0x7f) == 0x27 &&
      (fifo & 0xf0) == 0 && leds[0] == Config::LED_CURRENT &&
      leds[1] == Config::LED_CURRENT && clearPpgFifo();
  lastServiceMs = lastDataMs = millis();
  return valid;
}

void acceptPpgSample(uint32_t red, uint32_t ir, uint64_t sampleTimeUs, uint64_t arrivalUs) {
  const RawSample sample{sampleTimeUs, arrivalUs, red, ir, sampleGeneration.load()};
  // Never wait for analysis, printing, an FFT, or a model in the FIFO owner.
  if (xQueueSend(rawQueue, &sample, 0) != pdTRUE) {
    queueDrops.fetch_add(1);
    sampleGeneration.fetch_add(1); // Consumer rejects all older generations.
  }
}

void sensorTask(void*) {
  bool busReady = Wire.begin(Config::I2C_SDA, Config::I2C_SCL, Config::I2C_HZ);
  Wire.setTimeOut(20);
  uint32_t lastRetry = millis() - Config::RETRY_MS;
  for (;;) {
    const uint32_t now = millis();
    if (!sensorReady.load()) {
      if (now - lastRetry >= Config::RETRY_MS) {
        lastRetry = now;
        if (!busReady) busReady = Wire.begin(Config::I2C_SDA, Config::I2C_SCL, Config::I2C_HZ);
        sensorReady.store(busReady && initializePpg());
      }
      vTaskDelay(pdMS_TO_TICKS(Config::SENSOR_POLL_MS));
      continue;
    }
    uint8_t pointers[3] = {};
    const bool readOk = readPpgRegisters(0x04, pointers, 3);
    if (!readOk || (pointers[1] & 0x1f) || now - lastServiceMs >= Config::FIFO_DEADLINE_MS) {
      markSensorGap();
      sensorReady.store(readOk && clearPpgFifo());
      lastServiceMs = lastDataMs = now;
      vTaskDelay(pdMS_TO_TICKS(Config::SENSOR_POLL_MS));
      continue;
    }
    lastServiceMs = now;
    const uint8_t pending = (pointers[0] - pointers[2]) & 0x1f;
    if (!pending && now - lastDataMs >= Config::NO_SAMPLE_MS) {
      markSensorGap(); sensorReady.store(false);
    }
    const uint64_t batchUs = static_cast<uint64_t>(esp_timer_get_time());
    for (uint8_t i = 0; i < pending; ++i) {
      uint8_t bytes[6] = {};
      if (!readPpgRegisters(0x07, bytes, sizeof(bytes))) {
        markSensorGap(); sensorReady.store(false); break;
      }
      const uint32_t red = ((uint32_t(bytes[0]) << 16) | (uint32_t(bytes[1]) << 8) | bytes[2]) & 0x3ffff;
      const uint32_t ir = ((uint32_t(bytes[3]) << 16) | (uint32_t(bytes[4]) << 8) | bytes[5]) & 0x3ffff;
      const uint64_t age = uint64_t(pending - 1 - i) * Config::SAMPLE_US;
      // Reconstruct the sensor's nominal clock; polling jitter is not a sample gap.
      lastSampleUs = lastSampleUs ? lastSampleUs + Config::SAMPLE_US : batchUs - age;
      acceptPpgSample(red, ir, lastSampleUs, batchUs - age);
      lastDataMs = now;
    }
    vTaskDelay(pdMS_TO_TICKS(Config::SENSOR_POLL_MS));
  }
}
