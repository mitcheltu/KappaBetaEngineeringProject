#pragma once
#include <Arduino.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include "FirmwareConfig.h"
#include "DemoSequence.h"
#include "SensorTelemetry.h"
#include "HistoryBuffer.h"
#include "ScenarioProtocol.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "Select ESP32S3 Dev Module."
#endif

// Only the Arduino loop owns sensor values. BLE callbacks enqueue commands;
// the independent control task shares only atomic snapshots and stop requests.
extern SensorReadings sensors;
extern History::Buffer historyBuffer;
extern std::atomic<bool> zeroRequested, stopRequested, sosReady;
extern std::atomic<bool> pumpOutputReady;
extern std::atomic<uint16_t> batteryMv;
extern std::atomic<uint32_t> batteryDiagnostic, actuatorState;
extern bool controlReady;
extern TaskHandle_t controlTaskHandle;
// Scenario <-> control task handshake (Scenario.ino owns the controller; the control
// task owns DemoSequence and the GPIOs). Store order: control publishes sequenceInfo
// before scenarioTriggerAccepted; the scenario task loads them in the opposite order.
extern std::atomic<uint32_t> scenarioTriggerRequest, scenarioPlayingRun;
extern std::atomic<uint32_t> scenarioTriggerAccepted, scenarioTriggerRefused;
extern std::atomic<uint32_t> cancelEvents;     // Physical Cancel press, Serial off or double tap.
extern std::atomic<uint8_t> sequenceInfo;      // DemoSequence stage | trigger << 4.
extern std::atomic<bool> tapCancelRequested;   // Set by the sensor loop on a double tap.
extern std::atomic<uint32_t> doubleTapCount;

enum class LogLevel : uint8_t { ERROR, WARN, INFO, DEBUG };
bool initializeLogging();
void logMessage(LogLevel level, const char* tag, const char* format, ...)
    __attribute__((format(printf, 3, 4)));
void logReply(const char* tag, const char* format, ...)
    __attribute__((format(printf, 2, 3)));
void setLogLevel(int level);
void logStatus();
void logPpgDiagnostics();
void serviceDiagnostics(uint32_t now);
void serviceSerial();
void controlTask(void*);
void initializePumpOutput();
bool writePumpOutput(uint32_t now, bool requested);
void initializeSensors();
void serviceSensors(uint32_t now);
void initializeBle();
void serviceBle();
void publishTelemetry(uint32_t now);
bool bleLiveEnabled();
bool bleConnected();
void initializeHistory();
uint16_t historyFlags();
void recordDepthSample();
void recordImuSample();
void recordPpgSample(uint64_t sampleTimeUs);
void recordPpgGap();
void recordSurfaceZero();
void recordSystemSample(const SystemPacket& packet);
// Scenario playback (Scenario.ino).
bool initializeScenario();
bool queueScenarioCommand(const Scenario::Command& command, const char* origin);
bool scenarioStatus(Scenario::Status& out);
size_t scenarioCatalog(uint8_t* out, size_t capacity);  // CatalogHeader + entries.
uint16_t scenarioCatalogCrc();
void logScenarioCatalog();
