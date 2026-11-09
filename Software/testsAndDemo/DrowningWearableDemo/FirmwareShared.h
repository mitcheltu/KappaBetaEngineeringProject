#pragma once
#include <Arduino.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include "FirmwareConfig.h"
#include "DemoSequence.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "Select ESP32S3 Dev Module."
#endif

// Only the Arduino loop owns sensor values. BLE callbacks enqueue commands;
// the independent control task shares only atomic snapshots and stop requests.
struct SensorReadings {
  bool busReady = false, imuReady = false, depthReady = false, ppgReady = false;
  bool imuValid = false, pressureValid = false, rawPpgValid = false;
  bool hrValid = false, spo2Valid = false;
  float imuAxes[6] = {};
  float pressurePa = NAN, temperatureC = NAN, surfacePa = NAN;
  uint32_t imuMs = 0, pressureMs = 0, ppgMs = 0, estimateMs = 0;
  uint32_t red = 0, ir = 0, sensorErrors = 0, ppgOverflows = 0;
  int32_t heartRate = 0, oxygenSaturation = 0;
};
extern SensorReadings sensors;
extern std::atomic<bool> zeroRequested, stopRequested, sosReady;
extern std::atomic<uint16_t> batteryMv;
extern std::atomic<uint32_t> batteryDiagnostic, actuatorState;
extern bool controlReady;
extern TaskHandle_t controlTaskHandle;

enum class LogLevel : uint8_t { ERROR, WARN, INFO, DEBUG };
bool initializeLogging();
void logMessage(LogLevel level, const char* tag, const char* format, ...)
    __attribute__((format(printf, 3, 4)));
void logReply(const char* tag, const char* format, ...)
    __attribute__((format(printf, 2, 3)));
void setLogLevel(int level);
void logStatus();
void serviceDiagnostics(uint32_t now);
void serviceSerial();
void controlTask(void*);
void initializeSensors();
void serviceSensors(uint32_t now);
void initializeBle();
void serviceBle();
void publishTelemetry(uint32_t now);
bool bleLiveEnabled();
bool bleConnected();
