#include "FirmwareShared.h"

namespace {
int ppgSignalCandidate = -1, ppgSignalReported = -1;
uint32_t ppgSignalChangedMs = 0;
uint32_t ppgMs = 0;
size_t windowCount = 0;
}

void logPpgSignalQuality(uint32_t red, uint32_t ir) {
  // Report a stable quality change, not every threshold crossing/sample.
  // This is an optical signal gate, not proof of skin contact.
  const int quality = ir >= Config::PPG_MAX_ADC || red >= Config::PPG_MAX_ADC ? 3 :
                      ir < Config::PPG_MIN_IR ? 1 : red == 0 ? 2 : 0;
  const uint32_t now = millis();
  if (quality != ppgSignalCandidate) {
    ppgSignalCandidate = quality;
    ppgSignalChangedMs = now;
  }
  if (quality == ppgSignalReported || now - ppgSignalChangedMs < 1000) return;
  ppgSignalReported = quality;
  const char* reason = quality == 0 ? "signal gate passed; collecting HR/SpO2 window" :
                       quality == 1 ? "IR below threshold; check contact/illumination" :
                       quality == 2 ? "red signal is zero" : "optical ADC near saturation";
  logMessage(LogLevel::INFO, "PPG", "%s (red=%lu IR=%lu)", reason,
             static_cast<unsigned long>(red), static_cast<unsigned long>(ir));
}

void resetPpgWindow() {
  if (windowCount != 0)
    logMessage(LogLevel::DEBUG, "PPG", "Discarding optical window (%u samples); raw/HR/SpO2 validity cleared", unsigned(windowCount));
  ppgSignalCandidate = ppgSignalReported = -1;
  windowCount = 0;
  const uint64_t timestampUs = static_cast<uint64_t>(esp_timer_get_time());
  ppgSensor.invalidateReading(timestampUs);
  sensorBuffers.pushPpg(ppgSensor.reading());
  if (processingTaskHandle)
    xTaskNotify(processingTaskHandle, PROCESSING_NOTIFY_PPG_UPDATED, eSetBits);
  recordPpgGap();
}

bool clearPpgFifo() {
  // Bypass the SparkFun four-slot software FIFO; check each hardware transfer.
  return writeRegister(Config::PPG_ADDRESS, 0x04, 0) &&
         writeRegister(Config::PPG_ADDRESS, 0x05, 0) &&
         writeRegister(Config::PPG_ADDRESS, 0x06, 0);
}

void acceptPpgSample(uint32_t red, uint32_t ir, uint64_t sampleTimeUs) {
  ppgMs = millis();
  ppgSensor.publishReading(red, ir, sampleTimeUs);
  sensorBuffers.pushPpg(ppgSensor.reading());
  logPpgSignalQuality(red, ir);
  if (!ppgSignalAccepted(red, ir)) {
    if (windowCount != 0)
      logMessage(LogLevel::DEBUG, "PPG", "Optical signal gate interrupted HR/SpO2 window (red=%lu IR=%lu)",
                 static_cast<unsigned long>(red), static_cast<unsigned long>(ir));
    windowCount = 0;
    if (processingTaskHandle)
      xTaskNotify(processingTaskHandle, PROCESSING_NOTIFY_PPG_UPDATED, eSetBits);
    recordPpgSample(sampleTimeUs);
    return;
  }
  ++windowCount;
  if (windowCount == Config::PPG_ANALYSIS_WINDOW_READINGS) {
    if (processingTaskHandle)
      xTaskNotify(processingTaskHandle,
                  PROCESSING_NOTIFY_PPG_UPDATED | PROCESSING_NOTIFY_PPG_ANALYZE,
                  eSetBits);
    // Advance the trigger by 25 readings. The worker copies the latest full
    // 100-reading window from the ring when it handles the notification.
    windowCount = Config::PPG_ANALYSIS_WINDOW_READINGS -
                  Config::PPG_ANALYSIS_ADVANCE_READINGS;
  }
  recordPpgSample(sampleTimeUs);
}

bool processPpgSamples(bool analyzeWindow) {
  static PpgReading snapshot[Config::PPG_ANALYSIS_WINDOW_READINGS]{};
  static uint32_t irWindow[Config::PPG_ANALYSIS_WINDOW_READINGS]{};
  static uint32_t redWindow[Config::PPG_ANALYSIS_WINDOW_READINGS]{};

  const uint64_t nowUs = static_cast<uint64_t>(esp_timer_get_time());
  PpgReading latest{};
  if (!sensorBuffers.latestPpg(nowUs, latest)) return false;

  if (!latest.valid || !ppgSignalAccepted(latest.red, latest.infrared)) {
    ppgSensor.invalidateAnalysis(latest.timestampUs);
    sensorBuffers.publishPpgAnalysis(ppgSensor.analysis());
    return false;
  }
  if (!analyzeWindow) return false;

  const std::size_t copied = sensorBuffers.copyLatestPpg(
      nowUs, snapshot, Config::PPG_ANALYSIS_WINDOW_READINGS);
  if (copied != Config::PPG_ANALYSIS_WINDOW_READINGS) return false;

  for (std::size_t i = 0; i < copied; ++i) {
    if (!snapshot[i].valid ||
        !ppgSignalAccepted(snapshot[i].red, snapshot[i].infrared)) {
      ppgSensor.invalidateAnalysis(latest.timestampUs);
      sensorBuffers.publishPpgAnalysis(ppgSensor.analysis());
      return false;
    }
    redWindow[i] = snapshot[i].red;
    irWindow[i] = snapshot[i].infrared;
  }
  const uint64_t windowStartUs = snapshot[0].timestampUs;
  const uint64_t windowEndUs = snapshot[copied - 1].timestampUs;

  int32_t heartRate = 0, oxygenSaturation = 0;
  int8_t validHr = 0, validSpo2 = 0;
  const bool previousHrValid = ppgSensor.analysis().heartRateValid;
  const bool previousSpo2Valid = ppgSensor.analysis().oxygenSaturationValid;
  maxim_heart_rate_and_oxygen_saturation(irWindow,
      Config::PPG_ANALYSIS_WINDOW_READINGS, redWindow,
      &oxygenSaturation, &validSpo2, &heartRate, &validHr);
  const bool hrValid = validHr != 0 && heartRate > 0 && heartRate < 6553;
  const bool spo2Valid = validSpo2 != 0 && oxygenSaturation > 0 &&
                         oxygenSaturation <= 100;
  ppgSensor.publishAnalysis(heartRate, oxygenSaturation, hrValid, spo2Valid,
                            windowStartUs, windowEndUs);
  sensorBuffers.publishPpgAnalysis(ppgSensor.analysis());
  if (hrValid != previousHrValid || spo2Valid != previousSpo2Valid)
    logMessage(LogLevel::DEBUG, "PPG", "Estimate validity HR=%s (%ld BPM), SpO2=%s (%ld%%)",
               hrValid ? "valid" : "invalid", static_cast<long>(heartRate),
               spo2Valid ? "valid" : "invalid", static_cast<long>(oxygenSaturation));
  return true;
}

float getPpgRisk() {
  // Placeholder until the PPG risk model and mapping are available.
  PpgReading latest{};
  const uint64_t nowUs = static_cast<uint64_t>(esp_timer_get_time());
  const uint64_t timestampUs = sensorBuffers.latestPpg(nowUs, latest)
      ? latest.timestampUs : nowUs;
  const PpgRiskResult result = ppgSensor.riskResult(timestampUs);
  return result.valid ? result.score : NAN;
}

void servicePpg() {
  if (!ppgReady) return;
  const uint32_t now = millis();
  uint8_t pointers[3]; // write pointer, overflow counter, read pointer
  if (!readRegisters(Config::PPG_ADDRESS, 0x04, pointers, sizeof(pointers))) {
    logMessage(LogLevel::WARN, "PPG", "MAX30102 FIFO pointer read failed; readings invalid until reinitialization");
    ++sensorErrors; resetPpgWindow(); ppgReady = false; return;
  }
  if ((pointers[1] & 0x1f) != 0 || now - lastFifoServiceMs >= 1000) {
    logMessage(LogLevel::WARN, "PPG", "FIFO discontinuity: overflow=%u serviceGap=%lums; discarding window",
               unsigned(pointers[1] & 0x1f), static_cast<unsigned long>(now - lastFifoServiceMs));
    // A discontinuous window must not produce an HR/SpO2 estimate.
    ppgOverflows += (pointers[1] & 0x1f) ? (pointers[1] & 0x1f) : 1;
    resetPpgWindow();
    if (!clearPpgFifo()) {
      logMessage(LogLevel::WARN, "PPG", "FIFO clear failed; waiting for reinitialization");
      ++sensorErrors; ppgReady = false;
    }
    lastFifoServiceMs = now; return;
  }
  lastFifoServiceMs = now;
  const uint8_t pending = (pointers[0] - pointers[2]) & 0x1f;
  if (pending == 0 && now - ppgMs >= 1000) {
    logMessage(LogLevel::WARN, "PPG", "No new samples for %lums; check MAX30102 connection/configuration",
               static_cast<unsigned long>(now - ppgMs));
    ++sensorErrors; resetPpgWindow(); ppgReady = false; return;
  }
  const uint64_t batchTimeUs = esp_timer_get_time();
  for (uint8_t i = 0; i < pending; ++i) {
    uint8_t sample[6];
    if (!readRegisters(Config::PPG_ADDRESS, 0x07, sample, sizeof(sample))) {
      logMessage(LogLevel::WARN, "PPG", "FIFO sample read failed at %u/%u; discarding window and reinitializing",
                 unsigned(i + 1), unsigned(pending));
      ++sensorErrors; resetPpgWindow(); ppgReady = false; return;
    }
    const uint32_t red = ((uint32_t(sample[0]) << 16) | (uint32_t(sample[1]) << 8) | sample[2]) & 0x3ffff;
    const uint32_t ir = ((uint32_t(sample[3]) << 16) | (uint32_t(sample[4]) << 8) | sample[5]) & 0x3ffff;
    const uint64_t samplePeriodUs =
        1000000ULL / Config::PPG_FIFO_READINGS_PER_SECOND;
    const uint64_t ageUs = uint64_t(pending - 1 - i) * samplePeriodUs;
    acceptPpgSample(red, ir, batchTimeUs > ageUs ? batchTimeUs - ageUs : 0);
  }
}

void initializePpg() {
  static bool failureReported = false;
  logMessage(LogLevel::DEBUG, "PPG", "Initializing MAX30102 at 0x%02X", unsigned(Config::PPG_ADDRESS));
  resetPpgWindow();
  if (!ppg.begin(Wire, Config::I2C_HZ, Config::PPG_ADDRESS)) {
    ++sensorErrors;
    const LogLevel level = failureReported ? LogLevel::DEBUG : LogLevel::WARN;
    uint8_t partId = 0;
    if (readRegisters(Config::PPG_ADDRESS, 0xff, &partId, 1))
      logMessage(level, "PPG", "MAX30102 init failed: part ID=0x%02X (expected 0x15); verify sensor model/configuration", unsigned(partId));
    else
      logMessage(level, "PPG", "MAX30102 init failed at 0x%02X: part ID read failed; check power/address/I2C wiring", unsigned(Config::PPG_ADDRESS));
    failureReported = true;
    return;
  }
  // 100 samples/s / 4-sample averaging = 25 Hz expected by Maxim algorithm.
  ppg.setup(60, Config::PPG_SAMPLE_AVERAGING, 2,
            Config::PPG_INTERNAL_SAMPLE_RATE_HZ, 411, 4096);
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
  if (!ppgReady) {
    ++sensorErrors;
    logMessage(failureReported ? LogLevel::DEBUG : LogLevel::WARN, "PPG",
               "MAX30102 config/FIFO check failed: mode=0x%02X spo2=0x%02X fifo=0x%02X LEDs=%u/%u",
               unsigned(mode), unsigned(spo2), unsigned(fifo), unsigned(led[0]), unsigned(led[1]));
    failureReported = true;
  } else {
    logMessage(LogLevel::INFO, "PPG", "MAX30102 ready: red+IR, 25 FIFO samples/s; HR/SpO2 need 100 accepted samples");
    failureReported = false;
  }
}
