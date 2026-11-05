#include "FirmwareShared.h"

void resetPpgWindow() {
  windowCount = 0;
  rawPpgValid = hrValid = spo2Valid = false;
  recordPpgGap();
}

bool clearPpgFifo() {
  // Bypass the SparkFun four-slot software FIFO; check each hardware transfer.
  return writeRegister(Config::PPG_ADDRESS, 0x04, 0) &&
         writeRegister(Config::PPG_ADDRESS, 0x05, 0) &&
         writeRegister(Config::PPG_ADDRESS, 0x06, 0);
}

void acceptPpgSample(uint32_t red, uint32_t ir, uint64_t sampleTimeUs) {
  redValue = red; irValue = ir; ppgMs = millis(); rawPpgValid = true;
  if (ir < Config::PPG_MIN_IR || red == 0 || ir >= Config::PPG_MAX_ADC || red >= Config::PPG_MAX_ADC) {
    windowCount = 0; hrValid = spo2Valid = false;
    recordPpgSample(sampleTimeUs);
    return;
  }
  irWindow[windowCount] = ir;
  redWindow[windowCount++] = red;
  if (windowCount < 100) { recordPpgSample(sampleTimeUs); return; }
  int8_t validHr = 0, validSpo2 = 0;
  maxim_heart_rate_and_oxygen_saturation(irWindow, 100, redWindow,
      &oxygenSaturation, &validSpo2, &heartRate, &validHr);
  hrValid = validHr != 0 && heartRate > 0 && heartRate < 6553;
  spo2Valid = validSpo2 != 0 && oxygenSaturation > 0 && oxygenSaturation <= 100;
  estimateMs = millis();
  // Four seconds at 25 FIFO samples/s, recomputed each second (Example8).
  memmove(irWindow, irWindow + 25, 75 * sizeof(uint32_t));
  memmove(redWindow, redWindow + 25, 75 * sizeof(uint32_t));
  windowCount = 75;
  recordPpgSample(sampleTimeUs);
}

void servicePpg() {
  if (!ppgReady) return;
  const uint32_t now = millis();
  uint8_t pointers[3]; // write pointer, overflow counter, read pointer
  if (!readRegisters(Config::PPG_ADDRESS, 0x04, pointers, sizeof(pointers))) {
    ++sensorErrors; resetPpgWindow(); ppgReady = false; return;
  }
  if ((pointers[1] & 0x1f) != 0 || now - lastFifoServiceMs >= 1000) {
    // A discontinuous window must not produce an HR/SpO2 estimate.
    ppgOverflows += (pointers[1] & 0x1f) ? (pointers[1] & 0x1f) : 1;
    resetPpgWindow();
    if (!clearPpgFifo()) { ++sensorErrors; ppgReady = false; }
    lastFifoServiceMs = now; return;
  }
  lastFifoServiceMs = now;
  const uint8_t pending = (pointers[0] - pointers[2]) & 0x1f;
  if (pending == 0 && now - ppgMs >= 1000) {
    ++sensorErrors; resetPpgWindow(); ppgReady = false; return;
  }
  const uint64_t batchTimeUs = esp_timer_get_time();
  for (uint8_t i = 0; i < pending; ++i) {
    uint8_t sample[6];
    if (!readRegisters(Config::PPG_ADDRESS, 0x07, sample, sizeof(sample))) {
      ++sensorErrors; resetPpgWindow(); ppgReady = false; return;
    }
    const uint32_t red = ((uint32_t(sample[0]) << 16) | (uint32_t(sample[1]) << 8) | sample[2]) & 0x3ffff;
    const uint32_t ir = ((uint32_t(sample[3]) << 16) | (uint32_t(sample[4]) << 8) | sample[5]) & 0x3ffff;
    const uint64_t ageUs = uint64_t(pending - 1 - i) * 40000;
    acceptPpgSample(red, ir, batchTimeUs > ageUs ? batchTimeUs - ageUs : 0);
  }
}

void initializePpg() {
  resetPpgWindow();
  if (!ppg.begin(Wire, Config::I2C_HZ, Config::PPG_ADDRESS)) { ++sensorErrors; return; }
  // 100 samples/s / 4-sample averaging = 25 Hz expected by Maxim algorithm.
  ppg.setup(60, 4, 2, 100, 411, 4096); // red + IR, 18-bit, no green LED
  ppg.disableFIFORollover();
  // Configuration writes don't report errors: verify essential registers.
  uint8_t mode = 0, spo2 = 0, fifo = 0, led[2] = {};
  ppgReady = readRegisters(Config::PPG_ADDRESS, 0x09, &mode, 1) &&
             readRegisters(Config::PPG_ADDRESS, 0x0a, &spo2, 1) &&
             readRegisters(Config::PPG_ADDRESS, 0x08, &fifo, 1) &&
             readRegisters(Config::PPG_ADDRESS, 0x0c, led, 2) &&
             mode == 0x03 && (spo2 & 0x7f) == 0x27 && (fifo & 0xf0) == 0x40 &&
             led[0] == 60 && led[1] == 60 && clearPpgFifo();
  lastFifoServiceMs = millis();
  ppgMs = lastFifoServiceMs; // Start the no-new-samples timeout after setup.
  if (!ppgReady) ++sensorErrors;
}
