#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <MAX30105.h> // SparkFun library also supports MAX30102.
#include <spo2_algorithm.h>
#include <NimBLEDevice.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <esp_timer.h>
#include "FirmwareConfig.h"
#include "Diagnostics.h"
#include "TelemetryProtocol.h"
#include "BenchSequence.h"
#include "ImuRiskEvaluator.h"
#include "ImuSensor.h"
#include "ImuWindow.h"
#include "DepthSensor.h"
#include "SinkRateEstimator.h"
#include "PpgSensor.h"
#include "RiskProcessing.h"
#include "SensorBuffers.h"
#include "HistoryProtocol.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "Select ESP32S3 Dev Module / ESP32-S3-DevKitC-1."
#endif

extern ImuSensor imu;
extern ImuRisk::Evaluator imuRiskEvaluator;
extern ImuRisk::Window imuWindow;
extern MAX30105 ppg;
extern PpgSensor ppgSensor;
extern DepthSensor depthSensor;
extern SinkRateEstimator depthRiskEstimator;
extern SensorBuffers sensorBuffers;
extern NimBLEServer* bleServer;
extern NimBLECharacteristic *depthCharacteristic, *imuCharacteristic;
extern NimBLECharacteristic *ppgCharacteristic, *systemCharacteristic;
extern std::atomic<bool> busReady, imuReady, ppgReady, depthReady;
extern bool imuValid;
extern float imuAxes[6];
extern uint32_t imuMs;
extern std::atomic<uint32_t> ppgOverflows, sensorErrors;
extern uint32_t lastFifoServiceMs;
extern std::atomic<uint32_t> historyRecordDrops;
extern std::atomic<bool> zeroRequested, stopRequested, startRequested;
extern std::atomic<bool> gyroCalibrationRequested, gyroCalibrationClearRequested;
constexpr uint32_t PROCESSING_NOTIFY_IMU = 1U << 0;
constexpr uint32_t PROCESSING_NOTIFY_PPG_UPDATED = 1U << 1;
constexpr uint32_t PROCESSING_NOTIFY_PPG_ANALYZE = 1U << 2;
extern std::atomic<uint16_t> batteryMv;
// Coherent diagnostic sample: high 16 bits = ADC mV, low 16 = battery estimate
// before validity filtering. UINT32_MAX means no sample yet.
extern std::atomic<uint32_t> batteryDiagnostic;
extern std::atomic<bool> sosReady;
extern std::atomic<uint32_t> actuatorState;
extern bool controlReady;
extern TaskHandle_t controlTaskHandle;
extern TaskHandle_t acquisitionTaskHandle, processingTaskHandle;

bool readRegisters(uint8_t address, uint8_t reg, uint8_t* data, size_t count);
bool writeRegister(uint8_t address, uint8_t reg, uint8_t value);
void resetPpgWindow();
bool clearPpgFifo();
void acceptPpgSample(uint32_t red, uint32_t ir, uint64_t sampleTimeUs);
bool processPpgSamples(bool analyzeWindow);
float getPpgRisk();
void servicePpg();
void initializePpg();
void initializeImu();
void initializeDepth();
void sampleImu();
bool calibrateImuGyro();
void clearImuCalibration();
void acquisitionTask(void*);
void processingTask(void*);
void serviceDepth();
void serviceDepthZero();
void controlTask(void*);
void initializeBle();
void sendPacket(NimBLECharacteristic* characteristic, const void* packet);
void publishTelemetry(uint32_t now);
void processCommand(const char* command);
void serviceSerial();
void initializeHistory();
void createHistoryCharacteristics(NimBLEService* service);
void historyConnected(NimBLEServer* server, NimBLEConnInfo& info);
void historyDisconnected(NimBLEConnInfo& info);
void serviceHistory();
bool historyLiveEnabled();
uint16_t historyConnectionHandle();
uint16_t historyFlags();
void updateHistoryIdentity();
void* historyMlWorkspace(size_t index);
void recordHistory(uint8_t kind, uint8_t flags, uint64_t timeUs, const void* payload, size_t bytes);
void recordDepthSample();
void recordImuSample(uint64_t sampleTimeUs);
void recordPpgSample(uint64_t sampleTimeUs);
void recordPpgGap();
void recordSurfaceZero();
