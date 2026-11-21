// Standalone MAX30102 research sketch; no pump-start path and no BLE dependency.
#include "FirmwareShared.h"

QueueHandle_t rawQueue = nullptr, commandQueue = nullptr, statusQueue = nullptr;
std::atomic<uint32_t> sampleGeneration{0}, queueDrops{0}, sensorGaps{0};
std::atomic<bool> sensorReady{false};
static StaticQueue_t rawQueueControl, commandQueueControl, statusQueueControl;
alignas(RawSample) static uint8_t rawQueueStorage[Config::RAW_QUEUE_SAMPLES * sizeof(RawSample)];
alignas(AnalysisCommand) static uint8_t commandQueueStorage[4 * sizeof(AnalysisCommand)];
alignas(AnalysisStatus) static uint8_t statusQueueStorage[sizeof(AnalysisStatus)];

void setup() {
  digitalWrite(Config::PUMP_GATE, LOW); pinMode(Config::PUMP_GATE, OUTPUT);
  digitalWrite(Config::VIBRATION_GATE, LOW); pinMode(Config::VIBRATION_GATE, OUTPUT);
  pinMode(Config::ACK_BUTTON, INPUT_PULLUP);
  rgbLedWrite(Config::ALARM_LED_PIN, 0, 0, 0);
  Serial.begin(115200);
  rawQueue = xQueueCreateStatic(Config::RAW_QUEUE_SAMPLES, sizeof(RawSample), rawQueueStorage, &rawQueueControl);
  commandQueue = xQueueCreateStatic(4, sizeof(AnalysisCommand), commandQueueStorage, &commandQueueControl);
  statusQueue = xQueueCreateStatic(1, sizeof(AnalysisStatus), statusQueueStorage, &statusQueueControl);
  TaskHandle_t analysisHandle = nullptr;
  const bool analysisOk = rawQueue && commandQueue && statusQueue &&
      xTaskCreatePinnedToCore(analysisTask, "ppg-analysis", Config::ANALYSIS_STACK_BYTES,
          nullptr, Config::ANALYSIS_PRIORITY, &analysisHandle, Config::ANALYSIS_CORE) == pdPASS;
  const bool sensorOk = analysisOk &&
      xTaskCreatePinnedToCore(sensorTask, "ppg-sensor", Config::SENSOR_STACK_BYTES,
          nullptr, Config::SENSOR_PRIORITY, nullptr, Config::SENSOR_CORE) == pdPASS;
  if (!sensorOk) {
    if (analysisHandle) vTaskDelete(analysisHandle);
    rgbLedWrite(Config::ALARM_LED_PIN, 40, 0, 40);
    Serial.println("PPG task/queue creation failed; restart after checking memory.");
    return;
  }
  Serial.println("MAX30102: raw 100 Hz; Maxim 25 Hz; oxygen 1 s; pulse 16 s / 2 s.");
  Serial.println("Research outputs only. Pulse gate starts unqualified. Commands: help | status | ack | revoke | qualify <wearer-id> <seconds>");
}

void loop() {
  serviceSerial();
  delay(10);
}
