// Final ESP32-S3 integration firmware. Pins: FirmwareConfig.h.
// Wiring: ../../Electrical/README.md.
// SOS: blue LED 10s, then pump until Cancel. Serial: help, status, z,
// calibrate gyro, clear calibration, off, sos, log debug.
#include "FirmwareShared.h"

ImuSensor imu;
ImuRisk::Evaluator imuRiskEvaluator(Config::CNN_SCORE_THRESHOLD);
ImuRisk::Window imuWindow;
MAX30105 ppg;
PpgSensor ppgSensor(Config::PPG_DUMMY_RISK_SCORE);
DepthSensor depthSensor(DepthSensorConfig{
    Config::DEPTH_SAMPLE_PERIOD_MS, Config::DEPTH_WATER_DENSITY_KG_M3});
SensorBuffers sensorBuffers;
NimBLEServer* bleServer = nullptr;
NimBLECharacteristic *depthCharacteristic = nullptr, *imuCharacteristic = nullptr;
NimBLECharacteristic *ppgCharacteristic = nullptr, *systemCharacteristic = nullptr;
std::atomic<bool> busReady{false}, imuReady{false}, ppgReady{false}, depthReady{false};
bool imuValid = false, rawPpgValid = false;
bool hrValid = false, spo2Valid = false;
float imuAxes[6] = {};
uint32_t imuMs = 0, ppgMs = 0, estimateMs = 0;
uint32_t redValue = 0, irValue = 0;
std::atomic<uint32_t> ppgOverflows{0}, sensorErrors{0};
int32_t heartRate = 0, oxygenSaturation = 0;
uint32_t irWindow[Config::PPG_ANALYSIS_WINDOW_READINGS] = {};
uint32_t redWindow[Config::PPG_ANALYSIS_WINDOW_READINGS] = {};
size_t windowCount = 0;
uint32_t lastFifoServiceMs = 0;
std::atomic<bool> zeroRequested{false}, stopRequested{false}, startRequested{false};
std::atomic<bool> gyroCalibrationRequested{false}, gyroCalibrationClearRequested{false};
std::atomic<uint16_t> batteryMv{UINT16_MAX};
std::atomic<uint32_t> batteryDiagnostic{UINT32_MAX};
std::atomic<bool> sosReady{false};
// Low byte: system flags 2..6; bits 8..23: countdown ms. One coherent snapshot.
std::atomic<uint32_t> actuatorState{0};
bool controlReady = false;
TaskHandle_t controlTaskHandle = nullptr;
TaskHandle_t acquisitionTaskHandle = nullptr, processingTaskHandle = nullptr;

void setup() {
  // Active-high low-side gates, with Rev F external 100k pulldowns.
  digitalWrite(Config::PUMP_GATE, LOW); pinMode(Config::PUMP_GATE, OUTPUT);
  digitalWrite(Config::VIBRATION_GATE, LOW); pinMode(Config::VIBRATION_GATE, OUTPUT);
  rgbLedWrite(Config::COUNTDOWN_LED_PIN, 0, 0, 0);
  pinMode(Config::CANCEL_BUTTON, INPUT_PULLUP); pinMode(Config::SOS_BUTTON, INPUT_PULLUP);
  pinMode(Config::IMU_INT1, INPUT); pinMode(Config::PPG_INT, INPUT); // Wired, but polled.
  analogReadResolution(12); analogSetPinAttenuation(Config::BATTERY_ADC, ADC_11db);
  Serial.begin(115200);
  const bool loggingReady = initializeLogging();
  const bool buffersReady = sensorBuffers.begin();
  controlReady = xTaskCreate(controlTask, "bench-test", 4096, nullptr, 3, &controlTaskHandle) == pdPASS;
  // The only direct print is a startup fallback if no writer could be created.
  if (!loggingReady) Serial.println("ERROR: diagnostic queue/task unavailable; logging disabled.");
  if (!buffersReady) logMessage(LogLevel::ERROR, "SENSORS", "Sensor buffer mutex allocation failed; acquisition tasks will not start.");
  logMessage(LogLevel::INFO, "BOOT", "Final integration: IMU 20Hz/3s window; SOS -> blue LED 10s -> pump until Cancel/off");
  logMessage(controlReady ? LogLevel::INFO : LogLevel::ERROR, "CONTROL",
      "task=%s enabled=%d; active-low SOS=GPIO%d Cancel=GPIO%d; blue LED=GPIO%d pump=GPIO%d; vibration held off",
      controlReady ? "ready" : "FAILED (outputs off)", Config::ENABLE_ACTUATOR_TESTS,
      Config::SOS_BUTTON, Config::CANCEL_BUTTON, Config::COUNTDOWN_LED_PIN, Config::PUMP_GATE);
  logMessage(LogLevel::INFO, "BATTERY", "GPIO%d ADC x %.2f; valid=%u..%u mV; SOS requires >=%u mV; high readings also block",
      Config::BATTERY_ADC, Config::BATTERY_DIVIDER_RATIO, Config::BATTERY_VALID_MIN_MV,
      Config::BATTERY_VALID_MAX_MV, Config::ACTUATOR_MIN_BATTERY_MV);
  initializeHistory();
  initializeBle();

  if (buffersReady) {
    const bool processingCreated = xTaskCreatePinnedToCore(
        processingTask, "sensor-processing", Config::PROCESSING_TASK_STACK_BYTES,
        nullptr, Config::PROCESSING_TASK_PRIORITY, &processingTaskHandle,
        Config::PROCESSING_TASK_CORE) == pdPASS;
    const bool acquisitionCreated = processingCreated && xTaskCreatePinnedToCore(
        acquisitionTask, "sensor-acquisition", Config::ACQUISITION_TASK_STACK_BYTES,
        nullptr, Config::ACQUISITION_TASK_PRIORITY, &acquisitionTaskHandle,
        Config::ACQUISITION_TASK_CORE) == pdPASS;
    if (!processingCreated || !acquisitionCreated) {
      if (processingTaskHandle) {
        vTaskDelete(processingTaskHandle);
        processingTaskHandle = nullptr;
      }
      logMessage(LogLevel::ERROR, "SENSORS", "Acquisition/processing task creation failed; no background sensor work is running.");
    } else {
      logMessage(LogLevel::INFO, "SENSORS", "Acquisition and processing tasks started (I2C owner=acquisition; IMU=%lu Hz, depth=%lu Hz, PPG service=%lu ms).",
          static_cast<unsigned long>(Config::IMU_SAMPLE_RATE_HZ),
          static_cast<unsigned long>(1000 / Config::DEPTH_SAMPLE_PERIOD_MS),
          static_cast<unsigned long>(Config::PPG_SERVICE_PERIOD_MS));
    }
  }
  logMessage(LogLevel::INFO, "BOOT", "setup complete; commands: help, status, z, calibrate gyro, clear calibration, off, sos, log info/debug/warn/error/off");
}

void loop() {
  static uint32_t lastTelemetry = 0;
  serviceSerial();
  serviceHistory();
  const uint32_t now = millis();
  if (now - lastTelemetry >= Config::TELEMETRY_PERIOD_MS) {
    lastTelemetry = now;
    publishTelemetry(now);
  }
  serviceDiagnostics(millis());
  delay(1);
}

void acquisitionTask(void*) {
  uint64_t nowUs = static_cast<uint64_t>(esp_timer_get_time());
  uint64_t nextImuUs = nowUs;
  uint64_t nextPpgServiceUs = nowUs;
  uint64_t nextRetryUs = nowUs;
  for (;;) {
    nowUs = static_cast<uint64_t>(esp_timer_get_time());

    if (gyroCalibrationClearRequested.exchange(false)) {
      imu.clearCalibration();
      imuValid = false;
      for (float& axis : imuAxes) axis = NAN;
      ImuReading marker{};
      marker.timestampUs = nowUs;
      marker.timestampMs = millis();
      sensorBuffers.pushImu(marker);
      logMessage(LogLevel::INFO, "IMU", "Calibration cleared by acquisition task.");
    }

    if (gyroCalibrationRequested.exchange(false)) {
      if (imu.startGyroCalibration()) {
        imuValid = false;
        for (float& axis : imuAxes) axis = NAN;
        logMessage(LogLevel::INFO, "IMU", "Nonblocking gyro calibration started; keep the board motionless.");
      } else {
        logMessage(LogLevel::WARN, "IMU", "Gyro calibration could not start; sensor unavailable or calibration already active.");
      }
    }

    if (zeroRequested.load()) serviceDepthZero();

    if (nowUs >= nextRetryUs) {
      nextRetryUs = nowUs + 5000000ULL;
      if (!busReady.load()) {
        busReady = Wire.begin(Config::I2C_SDA, Config::I2C_SCL, Config::I2C_HZ);
        if (busReady.load()) Wire.setTimeOut(20);
        if (!busReady.load()) ++sensorErrors;
        logMessage(busReady.load() ? LogLevel::INFO : LogLevel::WARN, "I2C",
                   "bus initialization %s: SDA=%d SCL=%d speed=%lu Hz",
                   busReady.load() ? "ready" : "failed", Config::I2C_SDA,
                   Config::I2C_SCL, static_cast<unsigned long>(Config::I2C_HZ));
      }
      if (busReady.load() && !imuReady.load()) {
        initializeImu();
        // Reinitializing the BMI270 can delay FIFO service; discard any PPG
        // window spanning that interval before resuming optical analysis.
        if (ppgReady.load()) {
          resetPpgWindow();
          if (!clearPpgFifo()) ppgReady = false;
          lastFifoServiceMs = millis();
        }
      }
      if (busReady.load() && !depthReady.load()) initializeDepth();
      if (busReady.load() && !ppgReady.load()) initializePpg();
    }

    nowUs = static_cast<uint64_t>(esp_timer_get_time());
    if (nowUs >= nextPpgServiceUs) {
      servicePpg();
      nextPpgServiceUs = nowUs +
          static_cast<uint64_t>(Config::PPG_SERVICE_PERIOD_MS) * 1000ULL;
    }
    serviceDepth();

    const ImuCalibrationEvent calibrationEvent = imu.serviceGyroCalibration();
    if (calibrationEvent == ImuCalibrationEvent::COMPLETE) {
      const uint64_t markerTimeUs = static_cast<uint64_t>(esp_timer_get_time());
      ImuReading marker{};
      marker.timestampUs = markerTimeUs;
      marker.timestampMs = millis();
      imuValid = false;
      for (float& axis : imuAxes) axis = NAN;
      sensorBuffers.pushImu(marker);
      logMessage(LogLevel::INFO, "IMU", "Gyro calibration complete.");
    } else if (calibrationEvent == ImuCalibrationEvent::FAILED) {
      const uint64_t markerTimeUs = static_cast<uint64_t>(esp_timer_get_time());
      ImuReading marker{};
      marker.timestampUs = markerTimeUs;
      marker.timestampMs = millis();
      sensorBuffers.pushImu(marker);
      imuReady = false;
      imuValid = false;
      ++sensorErrors;
      logMessage(LogLevel::WARN, "IMU", "Gyro calibration sample failed; sensor reinitialization pending.");
    }

    nowUs = static_cast<uint64_t>(esp_timer_get_time());
    if (nowUs >= nextImuUs) {
      sampleImu();
      nextImuUs = nowUs + static_cast<uint64_t>(Config::IMU_PERIOD_MS) * 1000ULL;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
