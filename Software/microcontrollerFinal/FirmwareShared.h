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
#include "HistoryProtocol.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "Select ESP32S3 Dev Module / ESP32-S3-DevKitC-1."
#endif

extern ImuSensor imu;
extern ImuRisk::Evaluator imuRiskEvaluator;
extern ImuRisk::Window imuWindow;
extern MAX30105 ppg;
extern DepthSensor depthSensor;
extern NimBLEServer* bleServer;
extern NimBLECharacteristic *depthCharacteristic, *imuCharacteristic;
extern NimBLECharacteristic *ppgCharacteristic, *systemCharacteristic;
extern bool busReady, imuReady, ppgReady, depthReady;
extern bool imuValid, rawPpgValid;
extern bool hrValid, spo2Valid;
extern float imuAxes[6];
extern uint32_t imuMs, ppgMs, estimateMs;
extern uint32_t redValue, irValue, ppgOverflows, sensorErrors;
extern int32_t heartRate, oxygenSaturation;
extern uint32_t irWindow[100], redWindow[100];
extern size_t windowCount;
extern uint32_t lastFifoServiceMs;
extern std::atomic<bool> zeroRequested, stopRequested, startRequested;
extern std::atomic<uint16_t> batteryMv;
// Coherent diagnostic sample: high 16 bits = ADC mV, low 16 = battery estimate
// before validity filtering. UINT32_MAX means no sample yet.
extern std::atomic<uint32_t> batteryDiagnostic;
extern std::atomic<bool> sosReady;
extern std::atomic<uint32_t> actuatorState;
extern bool controlReady;
extern TaskHandle_t controlTaskHandle;

bool readRegisters(uint8_t address, uint8_t reg, uint8_t* data, size_t count);
bool writeRegister(uint8_t address, uint8_t reg, uint8_t value);
void resetPpgWindow();
bool clearPpgFifo();
void acceptPpgSample(uint32_t red, uint32_t ir, uint64_t sampleTimeUs);
void servicePpg();
void initializePpg();
void initializeImu();
void initializeDepth();
void sampleImu();
bool calibrateImuGyro();
void clearImuCalibration();
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
void recordImuSample();
void recordPpgSample(uint64_t sampleTimeUs);
void recordPpgGap();
void recordSurfaceZero();
