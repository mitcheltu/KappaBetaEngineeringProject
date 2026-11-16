#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <MAX30105.h>
#include <spo2_algorithm.h>
#include <esp_timer.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include "FirmwareConfig.h"
#include "PpgDecimator.h"
#include "PpgWindow.h"
#include "QualityGate.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "Select ESP32S3 Dev Module."
#endif

// timeUs is the reconstructed sensor clock; arrivalUs is the transport clock.
// Keep them separate: sensor oscillator drift must not make live data stale.
struct RawSample { uint64_t timeUs, arrivalUs; uint32_t red, ir, generation; };
enum class CommandKind : uint8_t { Acknowledge, Qualify, Revoke };
struct AnalysisCommand { CommandKind kind; uint32_t wearer, seconds; };
struct AnalysisStatus {
  int32_t hr, spo2;
  float concern, pulseScore;
  uint32_t analysisUs, worstAnalysisUs, samples, queueDrops, sensorGaps;
  uint32_t missedBudgets, wearer;
  bool hrValid, spo2Valid, oxygenActive, pulseActive, qualified, windowFull;
};
extern QueueHandle_t rawQueue, commandQueue, statusQueue;
extern std::atomic<uint32_t> sampleGeneration, queueDrops, sensorGaps;
extern std::atomic<bool> sensorReady;
bool readPpgRegisters(uint8_t reg, uint8_t* data, size_t count);
bool writePpgRegister(uint8_t reg, uint8_t value);
bool clearPpgFifo();
bool initializePpg();
void acceptPpgSample(uint32_t red, uint32_t ir, uint64_t sampleTimeUs, uint64_t arrivalUs);
void sensorTask(void*);
void analysisTask(void*);
void serviceSerial();
void updateIndicator(const AnalysisStatus& status);
