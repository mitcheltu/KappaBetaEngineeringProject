#include "FirmwareShared.h"

bool depthSensorPresent() {
  Wire.beginTransmission(Config::DEPTH_ADDRESS);
  const uint8_t result = Wire.endTransmission();
  if (result != 0)
    logMessage(LogLevel::DEBUG, "I2C", "MS5837 probe addr=0x%02X status=%u", unsigned(Config::DEPTH_ADDRESS), unsigned(result));
  return result == 0;
}

void initializeDepth() {
  static bool failureReported = false;
  logMessage(LogLevel::DEBUG, "DEPTH", "Initializing MS5837-30BA at 0x%02X", unsigned(Config::DEPTH_ADDRESS));
  pressureValid = false;
  pressurePa = temperatureC = NAN;
  surfacePa = NAN; // Reinitialization requires a new surface zero.
  // Official 1.1.1 uses the global Wire bus already configured in setup().
  pressureSensor.setModel(MS5837::MS5837_30BA);
  const bool present = depthSensorPresent();
  depthReady = present && pressureSensor.init();
  if (depthReady) {
    pressureSensor.setModel(MS5837::MS5837_30BA);
    pressureSensor.setFluidDensity(Config::FLUID_DENSITY_KG_M3);
    logMessage(LogLevel::INFO, "DEPTH", "MS5837-30BA ready; surface zero required ('z' at water surface)");
    failureReported = false;
  } else {
    ++sensorErrors;
    logMessage(failureReported ? LogLevel::DEBUG : LogLevel::WARN, "DEPTH",
               "MS5837-30BA init failed at 0x%02X: %s", unsigned(Config::DEPTH_ADDRESS),
               present ? "device ACKed but library initialization failed; check sensor model/PROM" : "no ACK; check power/address/I2C wiring");
    failureReported = true;
  }
}

void sampleDepth() {
  pressureValid = false;
  if (!depthReady) return;
  pressurePa = temperatureC = NAN;
  const bool presentBefore = depthSensorPresent();
  bool presentAfter = false;
  if (presentBefore) {
    // Upstream read() returns void and blocks ~40 ms. ACKs and range checks
    // detect missing devices/gross faults, not every failed internal transfer.
    pressureSensor.read();
    pressurePa = pressureSensor.pressure(MS5837::Pa);
    temperatureC = pressureSensor.temperature();
    presentAfter = depthSensorPresent();
    pressureValid = presentAfter &&
                    std::isfinite(pressurePa) && pressurePa > 0 && pressurePa <= 3000000 &&
                    std::isfinite(temperatureC) && temperatureC >= -20 && temperatureC <= 85;
  }
  if (!pressureValid) {
    if (!presentBefore || !presentAfter) {
      logMessage(LogLevel::WARN, "DEPTH", "MS5837 lost ACK %s conversion; readings and surface zero invalidated",
                 presentBefore ? "after" : "before");
    } else {
      logMessage(LogLevel::WARN, "DEPTH", "Invalid sample pressure=%.1fPa temperature=%.2fC; expected (0,3000000]Pa and [-20,85]C",
                 pressurePa, temperatureC);
    }
    pressurePa = temperatureC = NAN;
    ++sensorErrors; depthReady = false; surfacePa = NAN; recordDepthSample(); return;
  }
  pressureMs = millis();
  recordDepthSample();
}

void serviceDepthZero() {
  if (!zeroRequested.exchange(false)) return;
  logMessage(LogLevel::INFO, "DEPTH", "Surface zero requested; taking a fresh conversion");
  surfacePa = NAN;
  // Request a new conversion with the same presence/plausibility checks.
  servicePpg(); sampleDepth(); servicePpg();
  if (depthReady && pressureValid) {
    surfacePa = pressurePa;
    logMessage(LogLevel::INFO, "DEPTH", "Surface zero=%.1fPa; sensor must be at the water surface", surfacePa);
  } else logMessage(LogLevel::WARN, "DEPTH", "Surface zero failed: no valid pressure conversion");
  recordSurfaceZero();
}
