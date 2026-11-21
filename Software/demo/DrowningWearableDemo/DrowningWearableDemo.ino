// Rev J demo: SOS -> pulsed vibration 10 s -> pump 60 s (2 s ramp).
// Physical Cancel or an IMU double tap aborts either phase. Live sensor readings never
// start actuators; only SOS or on-device inference over a *scripted* scenario can.
#include "FirmwareShared.h"

SensorReadings sensors;
std::atomic<bool> zeroRequested{false}, stopRequested{false}, sosReady{false};
std::atomic<bool> pumpOutputReady{false};
std::atomic<uint16_t> batteryMv{UINT16_MAX};
std::atomic<uint32_t> batteryDiagnostic{UINT32_MAX}, actuatorState{0};
bool controlReady = false;
TaskHandle_t controlTaskHandle = nullptr;

void setup() {
  // ESP32 core 3.x requires GPIO ownership before digitalWrite(). Register
  // both pins as inputs, preload LOW, then enable their output drivers.
  // Rev J: GPIO13 drives DRV8871 IN1, GPIO7 drives the vibration MOSFET.
  pinMode(Config::PUMP_IN1, INPUT);
  pinMode(Config::VIBRATION_GATE, INPUT);
  digitalWrite(Config::PUMP_IN1, LOW);
  digitalWrite(Config::VIBRATION_GATE, LOW);
  pinMode(Config::PUMP_IN1, OUTPUT);
  pinMode(Config::VIBRATION_GATE, OUTPUT);
  pinMode(Config::CANCEL_BUTTON, INPUT_PULLUP);
  pinMode(Config::SOS_BUTTON, INPUT_PULLUP);
  pinMode(Config::IMU_INT1, INPUT);
  pinMode(Config::PPG_INT, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(Config::BATTERY_ADC, ADC_11db);
  Serial.begin(115200);
  const bool loggingReady = initializeLogging();
  initializePumpOutput();
  controlReady = xTaskCreate(controlTask, "demo-control", 4096, nullptr, 3,
                            &controlTaskHandle) == pdPASS;
  if (!loggingReady) Serial.println("Serial logger unavailable; outputs still use the control task.");
  logMessage(controlReady ? LogLevel::INFO : LogLevel::ERROR, "BOOT",
      "Rev J: control=%s motor=GPIO%d pump IN1=GPIO%d SOS=%d Cancel=%d; live sensors/history",
      controlReady ? "ready" : "FAILED (outputs off)", Config::VIBRATION_GATE,
      Config::PUMP_IN1, Config::SOS_BUTTON, Config::CANCEL_BUTTON);
  initializeHistory();
  initializeSensors();
  initializeScenario();
  initializeBle();
  logReply("BOOT", "Ready. SOS starts; Cancel/double tap/off stops. Serial: help, status, scenario, z, log info/debug/off");
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
