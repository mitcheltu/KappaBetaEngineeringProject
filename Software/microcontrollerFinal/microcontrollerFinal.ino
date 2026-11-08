// Final ESP32-S3 integration firmware. Pins: FirmwareConfig.h.
// Wiring: ../../Electrical/README.md.
// SOS: blue LED 10s, then pump until Cancel. Serial: help, status, z,
// calibrate gyro, clear calibration, off, sos, log debug.
#include "FirmwareShared.h"

ImuSensor imu;
ImuRisk::Evaluator imuRiskEvaluator;
ImuRisk::Window imuWindow;
MAX30105 ppg;
MS5837 pressureSensor;
NimBLEServer* bleServer = nullptr;
NimBLECharacteristic *depthCharacteristic = nullptr, *imuCharacteristic = nullptr;
NimBLECharacteristic *ppgCharacteristic = nullptr, *systemCharacteristic = nullptr;
bool busReady = false, imuReady = false, ppgReady = false, depthReady = false;
bool pressureValid = false, imuValid = false, rawPpgValid = false;
bool hrValid = false, spo2Valid = false;
float pressurePa = NAN, temperatureC = NAN, surfacePa = NAN;
float imuAxes[6] = {};
uint32_t pressureMs = 0, imuMs = 0, ppgMs = 0, estimateMs = 0;
uint32_t redValue = 0, irValue = 0, ppgOverflows = 0, sensorErrors = 0;
int32_t heartRate = 0, oxygenSaturation = 0;
uint32_t irWindow[100] = {}, redWindow[100] = {};
size_t windowCount = 0;
uint32_t lastFifoServiceMs = 0;
std::atomic<bool> zeroRequested{false}, stopRequested{false}, startRequested{false};
std::atomic<uint16_t> batteryMv{UINT16_MAX};
std::atomic<uint32_t> batteryDiagnostic{UINT32_MAX};
std::atomic<bool> sosReady{false};
// Low byte: system flags 2..6; bits 8..23: countdown ms. One coherent snapshot.
std::atomic<uint32_t> actuatorState{0};
bool controlReady = false;
TaskHandle_t controlTaskHandle = nullptr;

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
  controlReady = xTaskCreate(controlTask, "bench-test", 4096, nullptr, 3, &controlTaskHandle) == pdPASS;
  // The only direct print is a startup fallback if no writer could be created.
  if (!loggingReady) Serial.println("ERROR: diagnostic queue/task unavailable; logging disabled.");
  logMessage(LogLevel::INFO, "BOOT", "Final integration: IMU 20Hz/3s window; SOS -> blue LED 10s -> pump until Cancel/off");
  logMessage(controlReady ? LogLevel::INFO : LogLevel::ERROR, "CONTROL",
      "task=%s enabled=%d; active-low SOS=GPIO%d Cancel=GPIO%d; blue LED=GPIO%d pump=GPIO%d; vibration held off",
      controlReady ? "ready" : "FAILED (outputs off)", Config::ENABLE_ACTUATOR_TESTS,
      Config::SOS_BUTTON, Config::CANCEL_BUTTON, Config::COUNTDOWN_LED_PIN, Config::PUMP_GATE);
  logMessage(LogLevel::INFO, "BATTERY", "GPIO%d ADC x %.2f; valid=%u..%u mV; SOS requires >=%u mV; high readings also block",
      Config::BATTERY_ADC, Config::BATTERY_DIVIDER_RATIO, Config::BATTERY_VALID_MIN_MV,
      Config::BATTERY_VALID_MAX_MV, Config::ACTUATOR_MIN_BATTERY_MV);
  initializeHistory();
  busReady = Wire.begin(Config::I2C_SDA, Config::I2C_SCL, Config::I2C_HZ);
  Wire.setTimeOut(20);
  if (busReady) {
    initializeImu(); initializeDepth(); initializePpg(); // Optical sampling last.
  } else ++sensorErrors;
  logMessage(busReady ? LogLevel::INFO : LogLevel::ERROR, "I2C",
      "SDA=%d SCL=%d speed=%lu Hz bus=%d; BMI270(0x68)=%d MS5837(0x76)=%d MAX30102(0x57)=%d",
      Config::I2C_SDA, Config::I2C_SCL, static_cast<unsigned long>(Config::I2C_HZ),
      busReady, imuReady, depthReady, ppgReady);
  initializeBle();
  logMessage(LogLevel::INFO, "BOOT", "setup complete; commands: help, status, z, calibrate gyro, clear calibration, off, sos, log info/debug/warn/error/off");
}

void loop() {
  static uint32_t lastImu = 0, lastDepth = 0, lastTelemetry = 0, lastRetry = 0;
  serviceSerial(); servicePpg(); serviceDepthZero(); serviceHistory();
  uint32_t now = millis();
  if (now - lastImu >= Config::IMU_PERIOD_MS) { lastImu = now; sampleImu(); }
  if (now - lastDepth >= Config::DEPTH_PERIOD_MS) {
    lastDepth = now; servicePpg(); sampleDepth(); servicePpg();
  }
  now = millis();
  if (now - lastRetry >= 5000) {
    lastRetry = now;
    if (!busReady) {
      busReady = Wire.begin(Config::I2C_SDA, Config::I2C_SCL, Config::I2C_HZ);
      Wire.setTimeOut(20);
      if (!busReady) ++sensorErrors;
      logMessage(busReady ? LogLevel::INFO : LogLevel::WARN, "I2C", "bus restart %s", busReady ? "ready" : "failed");
    }
    // BMI reinitialization blocks: discard any affected optical window.
    if (busReady && !imuReady) {
      initializeImu();
      if (ppgReady) { resetPpgWindow(); if (!clearPpgFifo()) ppgReady = false; lastFifoServiceMs = millis(); }
    }
    if (busReady && !depthReady) initializeDepth();
    if (busReady && !ppgReady) initializePpg();
  }
  now = millis();
  if (now - lastTelemetry >= Config::TELEMETRY_PERIOD_MS) { lastTelemetry = now; publishTelemetry(now); }
  serviceHistory();
  serviceDiagnostics(millis());
  delay(1);
}
