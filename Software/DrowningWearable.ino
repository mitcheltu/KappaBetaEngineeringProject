/*
  Rev F sensor/manual bench-valve firmware. ESP32-S3-DevKitC-1-N8R8.
  No drowning detection, automatic deployment, servo, ECG or strobe logic.
  Libraries and wiring: README.md. Configuration: FirmwareConfig.h.
  SOS -> vibration 10s -> valve opens until Cancel. No sensor-based actuation.
  Serial 115200, newline commands: z, off, sos (same manual test as reed).
*/
#include <Arduino.h>
#include <Wire.h>
#include <MAX30105.h> // SparkFun library also supports MAX30102.
#include <spo2_algorithm.h>
#include <KappaBetaMS5837.h>
#include <NimBLEDevice.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include "FirmwareConfig.h"
#include "TelemetryProtocol.h"
#include "BenchSequence.h"
#include "CheckedBMI270.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "Select ESP32S3 Dev Module / ESP32-S3-DevKitC-1."
#endif

CheckedBMI270 imu;
MAX30105 ppg;
KappaBetaMS5837 pressureSensor;
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

// Explicit declarations avoid Arduino-generated custom-type prototype issues.
void publishTelemetry(uint32_t now);
void resetPpgWindow();
bool clearPpgFifo();

bool readRegisters(uint8_t address, uint8_t reg, uint8_t* data, size_t count) {
  Wire.beginTransmission(address);
  const bool written = Wire.write(reg) == 1;
  const uint8_t result = Wire.endTransmission(!written);
  if (!written || result != 0) return false;
  if (Wire.requestFrom(address, count, true) != count) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (!Wire.available()) return false;
    const int value = Wire.read();
    if (value < 0) return false;
    data[i] = static_cast<uint8_t>(value);
  }
  return true;
}

bool writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  const bool written = Wire.write(reg) == 1 && Wire.write(value) == 1;
  const uint8_t result = Wire.endTransmission();
  return written && result == 0;
}

void resetPpgWindow() {
  windowCount = 0;
  rawPpgValid = hrValid = spo2Valid = false;
}

bool clearPpgFifo() {
  // Bypass the SparkFun four-slot software FIFO; check each hardware transfer.
  return writeRegister(Config::PPG_ADDRESS, 0x04, 0) &&
         writeRegister(Config::PPG_ADDRESS, 0x05, 0) &&
         writeRegister(Config::PPG_ADDRESS, 0x06, 0);
}

void acceptPpgSample(uint32_t red, uint32_t ir) {
  redValue = red; irValue = ir; ppgMs = millis(); rawPpgValid = true;
  if (ir < Config::PPG_MIN_IR || red == 0 || ir >= Config::PPG_MAX_ADC || red >= Config::PPG_MAX_ADC) {
    windowCount = 0; hrValid = spo2Valid = false;
    return;
  }
  irWindow[windowCount] = ir;
  redWindow[windowCount++] = red;
  if (windowCount < 100) return;
  int8_t validHr = 0, validSpo2 = 0;
  maxim_heart_rate_and_oxygen_saturation(irWindow, 100, redWindow,
      &oxygenSaturation, &validSpo2, &heartRate, &validHr);
  hrValid = validHr != 0 && heartRate > 0 && heartRate < 6553;
  spo2Valid = validSpo2 != 0 && oxygenSaturation > 0 && oxygenSaturation <= 100;
  estimateMs = millis();
  // Four seconds at 25 FIFO samples/s, recomputed each second (Example8).
  memmove(irWindow, irWindow + 25, 75 * sizeof(uint32_t));
  memmove(redWindow, redWindow + 25, 75 * sizeof(uint32_t));
  windowCount = 75;
}

void servicePpg() {
  if (!ppgReady) return;
  const uint32_t now = millis();
  uint8_t pointers[3]; // write pointer, overflow counter, read pointer
  if (!readRegisters(Config::PPG_ADDRESS, 0x04, pointers, sizeof(pointers))) {
    ++sensorErrors; resetPpgWindow(); ppgReady = false; return;
  }
  if ((pointers[1] & 0x1f) != 0 || now - lastFifoServiceMs >= 1000) {
    // A discontinuous window must not produce an HR/SpO2 estimate.
    ppgOverflows += (pointers[1] & 0x1f) ? (pointers[1] & 0x1f) : 1;
    resetPpgWindow();
    if (!clearPpgFifo()) { ++sensorErrors; ppgReady = false; }
    lastFifoServiceMs = now; return;
  }
  lastFifoServiceMs = now;
  const uint8_t pending = (pointers[0] - pointers[2]) & 0x1f;
  if (pending == 0 && now - ppgMs >= 1000) {
    ++sensorErrors; resetPpgWindow(); ppgReady = false; return;
  }
  for (uint8_t i = 0; i < pending; ++i) {
    uint8_t sample[6];
    if (!readRegisters(Config::PPG_ADDRESS, 0x07, sample, sizeof(sample))) {
      ++sensorErrors; resetPpgWindow(); ppgReady = false; return;
    }
    const uint32_t red = ((uint32_t(sample[0]) << 16) | (uint32_t(sample[1]) << 8) | sample[2]) & 0x3ffff;
    const uint32_t ir = ((uint32_t(sample[3]) << 16) | (uint32_t(sample[4]) << 8) | sample[5]) & 0x3ffff;
    acceptPpgSample(red, ir);
  }
}

void initializePpg() {
  resetPpgWindow();
  if (!ppg.begin(Wire, Config::I2C_HZ, Config::PPG_ADDRESS)) { ++sensorErrors; return; }
  // 100 samples/s / 4-sample averaging = 25 Hz expected by Maxim algorithm.
  ppg.setup(60, 4, 2, 100, 411, 4096); // red + IR, 18-bit, no green LED
  ppg.disableFIFORollover();
  // Configuration writes don't report errors: verify essential registers.
  uint8_t mode = 0, spo2 = 0, fifo = 0, led[2] = {};
  ppgReady = readRegisters(Config::PPG_ADDRESS, 0x09, &mode, 1) &&
             readRegisters(Config::PPG_ADDRESS, 0x0a, &spo2, 1) &&
             readRegisters(Config::PPG_ADDRESS, 0x08, &fifo, 1) &&
             readRegisters(Config::PPG_ADDRESS, 0x0c, led, 2) &&
             mode == 0x03 && (spo2 & 0x7f) == 0x27 && (fifo & 0xf0) == 0x40 &&
             led[0] == 60 && led[1] == 60 && clearPpgFifo();
  lastFifoServiceMs = millis();
  ppgMs = lastFifoServiceMs; // Start the no-new-samples timeout after setup.
  if (!ppgReady) ++sensorErrors;
}

void initializeImu() {
  imuValid = false;
  imuReady = imu.begin(Wire, Config::IMU_ADDRESS);
  if (!imuReady) ++sensorErrors;
}

void initializeDepth() {
  pressureValid = false;
  surfacePa = NAN; // Reinitialization requires a new surface zero.
  depthReady = pressureSensor.init(Wire);
  if (depthReady) {
    pressureSensor.setModel(KappaBetaMS5837::MS5837_30BA);
    pressureSensor.setFluidDensity(Config::FLUID_DENSITY_KG_M3);
  } else ++sensorErrors;
}

void sampleImu() {
  if (!imuReady) return;
  imuValid = imu.read(imuAxes);
  if (imuValid) {
    for (size_t i = 0; i < 6; ++i) {
      const float scaled = imuAxes[i] * (i < 3 ? 1000.0f : 10.0f);
      imuValid = imuValid && std::isfinite(scaled) && scaled >= -32768 && scaled <= 32767;
    }
  }
  if (imuValid) imuMs = millis();
  else { ++sensorErrors; imuReady = false; }
}

void sampleDepth() {
  if (!depthReady) return;
  pressureValid = pressureSensor.read(); // Checked transactions, ~40 ms blocking.
  if (pressureValid) {
    pressurePa = pressureSensor.pressure(KappaBetaMS5837::Pa);
    temperatureC = pressureSensor.temperature();
    pressureValid = std::isfinite(pressurePa) && pressurePa > 0 && pressurePa <= 3000000 &&
                    std::isfinite(temperatureC) && temperatureC >= -20 && temperatureC <= 85;
  }
  if (!pressureValid) {
    ++sensorErrors; depthReady = false; surfacePa = NAN; return;
  }
  pressureMs = millis();
}

void serviceDepthZero() {
  if (!zeroRequested.exchange(false)) return;
  surfacePa = NAN;
  // Use a NEW checked conversion, never a cached reading.
  servicePpg(); sampleDepth(); servicePpg();
  if (depthReady && pressureValid) {
    surfacePa = pressurePa;
    Serial.printf("Surface zero: %.1f Pa. Zero only with sensor at the water surface.\n", surfacePa);
  } else Serial.println("Surface zero failed: no valid pressure conversion.");
}

void controlTask(void*) {
  BenchSequence sequence;
  uint32_t lastBattery = 0;
  bool sosRaw = digitalRead(Config::SOS_BUTTON) == LOW;
  bool sosStable = sosRaw; // A reed held at boot must not look like a new press.
  uint32_t sosChangedMs = millis();
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    const uint32_t now = millis();
    const bool cancel = digitalRead(Config::CANCEL_BUTTON) == LOW;
    const bool nextSos = digitalRead(Config::SOS_BUTTON) == LOW;
    if (nextSos != sosRaw) { sosRaw = nextSos; sosChangedMs = now; }
    if (now - sosChangedMs >= 30) sosStable = sosRaw;
    if (now - lastBattery >= 100) {
      lastBattery = now;
      uint32_t sum = 0;
      for (int i = 0; i < 8; ++i) sum += analogReadMilliVolts(Config::BATTERY_ADC);
      const uint32_t mv = lroundf((sum / 8.0f) * Config::BATTERY_DIVIDER_RATIO);
      batteryMv.store(mv >= 2500 && mv <= 4350 ? static_cast<uint16_t>(mv) : UINT16_MAX);
    }
    const uint16_t mv = batteryMv.load();
    const bool stop = stopRequested.exchange(false) || cancel;
    const bool start = startRequested.exchange(false) || sosStable;
    sequence.update(now, Config::ENABLE_ACTUATOR_TESTS, stop, start,
                    mv != UINT16_MAX && mv >= Config::VALVE_MIN_BATTERY_MV, 0);
    digitalWrite(Config::MOTOR_GATE, sequence.vibrationOn() ? HIGH : LOW);
    digitalWrite(Config::SOLENOID_GATE, sequence.valveOn() ? HIGH : LOW);
    const uint8_t flags = (sequence.valveOn() ? 4 : 0) | (cancel ? 8 : 0) |
        (sosStable ? 16 : 0) | (sequence.vibrationOn() ? 32 : 0) |
        (sequence.stage == BenchSequence::Stage::COUNTDOWN ? 64 : 0);
    actuatorState.store((sequence.remainingCountdownMs(now) << 8) | flags);
    // Independent of sensor/BLE blocking. Cancel acts on the first low level.
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(5));
  }
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override { NimBLEDevice::startAdvertising(); }
};
class ZeroDepthCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    const auto value = characteristic->getValue();
    if (value.size() == 1 && static_cast<uint8_t>(value[0]) == 1) zeroRequested.store(true);
  }
};
ServerCallbacks serverCallbacks;
ZeroDepthCallbacks zeroCallbacks;

void initializeBle() {
  NimBLEDevice::init(Config::DEVICE_NAME);
  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(&serverCallbacks, false);
  NimBLEService* service = bleServer->createService(Config::SERVICE_UUID);
  const uint32_t properties = NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY;
  depthCharacteristic = service->createCharacteristic(Config::DEPTH_UUID, properties, 20);
  imuCharacteristic = service->createCharacteristic(Config::IMU_UUID, properties, 20);
  ppgCharacteristic = service->createCharacteristic(Config::PPG_UUID, properties, 20);
  systemCharacteristic = service->createCharacteristic(Config::SYSTEM_UUID, properties, 20);
  NimBLECharacteristic* zero = service->createCharacteristic(Config::ZERO_DEPTH_UUID, NIMBLE_PROPERTY::WRITE, 1);
  zero->setCallbacks(&zeroCallbacks);
  publishTelemetry(millis());
  bleServer->start();
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->enableScanResponse(true);
  advertising->addServiceUUID(Config::SERVICE_UUID);
  advertising->setName(Config::DEVICE_NAME);
  advertising->start();
}

void sendPacket(NimBLECharacteristic* characteristic, const void* packet) {
  characteristic->setValue(static_cast<const uint8_t*>(packet), 20);
  if (bleServer->getConnectedCount() > 0) characteristic->notify();
}

void publishTelemetry(uint32_t now) {
  static uint16_t sequence = 0;
  const PacketHeader header{1, 0, sequence++, now};
  const bool freshPressure = pressureValid && now - pressureMs < 1000;
  const bool calibrated = freshPressure && std::isfinite(surfacePa);
  DepthPacket depth{};
  depth.header = header;
  depth.header.flags = (freshPressure ? 1 : 0) | (calibrated ? 2 : 0);
  depth.pressurePa = freshPressure ? pressurePa : NAN;
  depth.temperatureC = freshPressure ? temperatureC : NAN;
  depth.depthM = calibrated ? (pressurePa - surfacePa) / (Config::FLUID_DENSITY_KG_M3 * 9.80665f) : NAN;
  ImuPacket motion{};
  motion.header = header;
  if (imuValid && now - imuMs < 300) {
    motion.header.flags = 1;
    for (size_t i = 0; i < 6; ++i) motion.axes[i] = lroundf(imuAxes[i] * (i < 3 ? 1000.0f : 10.0f));
  }
  PpgPacket optical{};
  optical.header = header;
  optical.hrTenths = optical.spo2Hundredths = UINT16_MAX;
  if (rawPpgValid && now - ppgMs < 300) {
    optical.header.flags = 1; optical.red = redValue; optical.ir = irValue;
    if (now - estimateMs < 2000) {
      if (hrValid) { optical.header.flags |= 2; optical.hrTenths = heartRate * 10; }
      if (spo2Valid) { optical.header.flags |= 4; optical.spo2Hundredths = oxygenSaturation * 100; }
    }
  }
  SystemPacket system{};
  const uint32_t controls = actuatorState.load();
  system.header = header; system.batteryMv = batteryMv.load();
  system.header.flags = (system.batteryMv != UINT16_MAX ? 1 : 0) |
      (controlReady && Config::ENABLE_ACTUATOR_TESTS ? 2 : 0) |
      static_cast<uint8_t>(controls);
  system.countdownRemainingMs = static_cast<uint16_t>(controls >> 8);
  system.ppgOverflowCount = ppgOverflows; system.sensorErrors = sensorErrors;
  sendPacket(depthCharacteristic, &depth); sendPacket(imuCharacteristic, &motion);
  sendPacket(ppgCharacteristic, &optical); sendPacket(systemCharacteristic, &system);
}

void processCommand(const char* command) {
  if (strcmp(command, "z") == 0) { zeroRequested.store(true); return; }
  if (strcmp(command, "off") == 0) { stopRequested.store(true); Serial.println("Motor/valve stop requested."); return; }
  if (strcmp(command, "sos") == 0) {
    if (!controlReady || !Config::ENABLE_ACTUATOR_TESTS) {
      Serial.println("Actuator test disabled.");
    } else if ((actuatorState.load() & (4 | 8 | 64)) != 0 ||
               batteryMv.load() == UINT16_MAX || batteryMv.load() < Config::VALVE_MIN_BATTERY_MV) {
      Serial.println("SOS test rejected: already active, cancel or battery.");
    } else {
      startRequested.store(true);
      Serial.println("SOS test requested: vibration 10s, then valve open until Cancel/off.");
    }
    return;
  }
  Serial.println("Commands (newline): z = surface zero; sos = manual test; off = stop both outputs.");
}

void serviceSerial() {
  static char line[48];
  static size_t length = 0;
  static bool overflow = false;
  // Bound processing so a continuous stream cannot starve sensor servicing.
  for (int i = 0; i < 64 && Serial.available(); ++i) {
    const char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line[length] = '\0';
      if (!overflow && length > 0) processCommand(line);
      length = 0; overflow = false;
    } else if (length + 1 < sizeof(line) && !overflow) line[length++] = c;
    else overflow = true;
  }
}

void setup() {
  // Active-high low-side gates, with Rev F external 100k pulldowns.
  digitalWrite(Config::SOLENOID_GATE, LOW); pinMode(Config::SOLENOID_GATE, OUTPUT);
  digitalWrite(Config::MOTOR_GATE, LOW); pinMode(Config::MOTOR_GATE, OUTPUT);
  pinMode(Config::CANCEL_BUTTON, INPUT_PULLUP); pinMode(Config::SOS_BUTTON, INPUT_PULLUP);
  pinMode(Config::IMU_INT1, INPUT); pinMode(Config::PPG_INT, INPUT); // Wired, but polled.
  analogReadResolution(12); analogSetPinAttenuation(Config::BATTERY_ADC, ADC_11db);
  Serial.begin(115200);
  controlReady = xTaskCreate(controlTask, "bench-test", 3072, nullptr, 3, nullptr) == pdPASS;
  Serial.println("Rev F manual test: SOS vibrates 10s then opens valve until Cancel. No drowning logic.");
  if (!controlReady) Serial.println("Control task failed; motor and valve remain off.");
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
  serviceSerial(); servicePpg(); serviceDepthZero();
  uint32_t now = millis();
  if (now - lastImu >= 20) { lastImu = now; sampleImu(); }
  if (now - lastDepth >= 200) {
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
  delay(1);
}
