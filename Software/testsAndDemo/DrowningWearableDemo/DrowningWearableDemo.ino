// Rev I manual demonstration: SOS -> pulsed vibration 10 s -> pump 60 s.
// Physical Cancel aborts either phase. Sensor readings never start actuators.
#include "FirmwareShared.h"

SensorReadings sensors;
std::atomic<bool> zeroRequested{false}, stopRequested{false}, sosReady{false};
std::atomic<uint16_t> batteryMv{UINT16_MAX};
std::atomic<uint32_t> batteryDiagnostic{UINT32_MAX}, actuatorState{0};
bool controlReady = false;
TaskHandle_t controlTaskHandle = nullptr;

void setup() {
  // ESP32 core 3.x requires GPIO ownership before digitalWrite(). Register
  // both pins as inputs, preload LOW, then enable their output drivers.
  // Rev I's external gate pulldowns hold the MOSFETs off during this setup.
  pinMode(Config::PUMP_GATE, INPUT);
  pinMode(Config::VIBRATION_GATE, INPUT);
  digitalWrite(Config::PUMP_GATE, LOW);
  digitalWrite(Config::VIBRATION_GATE, LOW);
  pinMode(Config::PUMP_GATE, OUTPUT);
  pinMode(Config::VIBRATION_GATE, OUTPUT);
  pinMode(Config::CANCEL_BUTTON, INPUT_PULLUP);
  pinMode(Config::SOS_BUTTON, INPUT_PULLUP);
  pinMode(Config::IMU_INT1, INPUT);
  pinMode(Config::PPG_INT, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(Config::BATTERY_ADC, ADC_11db);
  Serial.begin(115200);
  const bool loggingReady = initializeLogging();
  controlReady = xTaskCreate(controlTask, "demo-control", 4096, nullptr, 3,
                            &controlTaskHandle) == pdPASS;
  if (!loggingReady) Serial.println("Serial logger unavailable; outputs still use the control task.");
  logMessage(controlReady ? LogLevel::INFO : LogLevel::ERROR, "BOOT",
      "Rev I DEMO: control=%s motor=GPIO%d pump=GPIO%d SOS=%d Cancel=%d; no automatic detection",
      controlReady ? "ready" : "FAILED (outputs off)", Config::VIBRATION_GATE,
      Config::PUMP_GATE, Config::SOS_BUTTON, Config::CANCEL_BUTTON);
  initializeSensors();
  initializeBle();
  logReply("BOOT", "Ready. Physical SOS starts; Cancel/off stops. Serial: help, status, z, log info/debug/off");
}

void loop() {
  static uint32_t lastTelemetry = 0;
  serviceSerial();
  serviceBle();
  serviceSensors(millis());
  const uint32_t now = millis();
  if (now - lastTelemetry >= Config::TELEMETRY_PERIOD_MS) {
    lastTelemetry = now;
    publishTelemetry(now);
  }
  serviceBle();
  serviceDiagnostics(now);
  delay(1);
}
