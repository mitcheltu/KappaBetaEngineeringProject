// Rev F ESP32-S3 bench firmware. Pins: FirmwareConfig.h. Wiring: ../Electrical/README.md.
// SOS: vibrate 10s, then pump until Cancel. Serial commands: z, off, sos.
#include "FirmwareShared.h"

CheckedBMI270 imu;
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
// Low byte: system flags 2..6; bits 8..23: countdown ms. One coherent snapshot.
std::atomic<uint32_t> actuatorState{0};
bool controlReady = false;

void setup() {
  // Active-high low-side gates, with Rev F external 100k pulldowns.
  digitalWrite(Config::PUMP_GATE, LOW); pinMode(Config::PUMP_GATE, OUTPUT);
  digitalWrite(Config::VIBRATION_GATE, LOW); pinMode(Config::VIBRATION_GATE, OUTPUT);
  pinMode(Config::CANCEL_BUTTON, INPUT_PULLUP); pinMode(Config::SOS_BUTTON, INPUT_PULLUP);
  pinMode(Config::IMU_INT1, INPUT); pinMode(Config::PPG_INT, INPUT); // Wired, but polled.
  analogReadResolution(12); analogSetPinAttenuation(Config::BATTERY_ADC, ADC_11db);
  Serial.begin(115200);
  controlReady = xTaskCreate(controlTask, "bench-test", 3072, nullptr, 3, nullptr) == pdPASS;
  Serial.println("Rev F manual test: SOS vibrates 10s then runs pump until Cancel. No drowning logic.");
  if (!controlReady) Serial.println("Control task failed; vibration and pump remain off.");
  initializeHistory();
  busReady = Wire.begin(Config::I2C_SDA, Config::I2C_SCL, Config::I2C_HZ);
  Wire.setTimeOut(20);
  if (busReady) {
    initializeImu(); initializeDepth(); initializePpg(); // Optical sampling last.
  } else ++sensorErrors;
  Serial.printf("I2C SDA=8 SCL=9: BMI270(68)=%d MS5837(76)=%d MAX30102(57)=%d\n", imuReady, depthReady, ppgReady);
  initializeBle();
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
  delay(1);
}
