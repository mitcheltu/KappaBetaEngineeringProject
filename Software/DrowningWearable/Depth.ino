#include "FirmwareShared.h"

bool depthSensorPresent() {
  Wire.beginTransmission(Config::DEPTH_ADDRESS);
  return Wire.endTransmission() == 0;
}

void initializeDepth() {
  pressureValid = false;
  pressurePa = temperatureC = NAN;
  surfacePa = NAN; // Reinitialization requires a new surface zero.
  // Official 1.1.1 uses the global Wire bus already configured in setup().
  pressureSensor.setModel(MS5837::MS5837_30BA);
  depthReady = depthSensorPresent() && pressureSensor.init();
  if (depthReady) {
    pressureSensor.setModel(MS5837::MS5837_30BA);
    pressureSensor.setFluidDensity(Config::FLUID_DENSITY_KG_M3);
  } else ++sensorErrors;
}

void sampleDepth() {
  pressureValid = false;
  if (!depthReady) return;
  pressurePa = temperatureC = NAN;
  if (depthSensorPresent()) {
    // Upstream read() returns void and blocks ~40 ms. ACKs and range checks
    // detect missing devices/gross faults, not every failed internal transfer.
    pressureSensor.read();
    pressurePa = pressureSensor.pressure(MS5837::Pa);
    temperatureC = pressureSensor.temperature();
    pressureValid = depthSensorPresent() &&
                    std::isfinite(pressurePa) && pressurePa > 0 && pressurePa <= 3000000 &&
                    std::isfinite(temperatureC) && temperatureC >= -20 && temperatureC <= 85;
  }
  if (!pressureValid) {
    pressurePa = temperatureC = NAN;
    ++sensorErrors; depthReady = false; surfacePa = NAN; recordDepthSample(); return;
  }
  pressureMs = millis();
  recordDepthSample();
}

void serviceDepthZero() {
  if (!zeroRequested.exchange(false)) return;
  surfacePa = NAN;
  // Request a new conversion with the same presence/plausibility checks.
  servicePpg(); sampleDepth(); servicePpg();
  if (depthReady && pressureValid) {
    surfacePa = pressurePa;
    Serial.printf("Surface zero: %.1f Pa. Zero only with sensor at the water surface.\n", surfacePa);
  } else Serial.println("Surface zero failed: no valid pressure conversion.");
  recordSurfaceZero();
}
