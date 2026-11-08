#include "FirmwareShared.h"

void initializeDepth() {
  static bool failureReported = false;
  logMessage(LogLevel::DEBUG, "DEPTH", "Initializing MS5837-30BA at 0x%02X", unsigned(Config::DEPTH_ADDRESS));
  depthReady = depthSensor.begin(Wire, Config::DEPTH_ADDRESS);
  if (depthReady) {
    logMessage(LogLevel::INFO, "DEPTH", "MS5837-30BA ready; surface zero required ('z' at water surface)");
    failureReported = false;
  } else {
    ++sensorErrors;
    logMessage(failureReported ? LogLevel::DEBUG : LogLevel::WARN, "DEPTH",
               "MS5837-30BA init failed at 0x%02X; check power/address/I2C wiring or PROM CRC",
               unsigned(Config::DEPTH_ADDRESS));
    failureReported = true;
  }
}

void sampleDepth() {
  if (!depthReady) return;
  if (!depthSensor.update()) {
    const DepthReading& reading = depthSensor.reading();
    logMessage(LogLevel::WARN,
               "DEPTH",
               "MS5837 sample invalid: pressure=%.1fPa temperature=%.2fC quality=0x%04x",
               reading.pressurePa, reading.temperatureC, unsigned(reading.qualityFlags));
    ++sensorErrors;
    depthReady = false;
  }
  recordDepthSample();
}

void serviceDepthZero() {
  if (!zeroRequested.exchange(false)) return;
  logMessage(LogLevel::INFO, "DEPTH", "Surface zero requested; taking a fresh conversion");
  servicePpg();
  const bool started = depthSensor.startDive();
  servicePpg();
  depthReady = depthSensor.ready();
  if (started) {
    logMessage(LogLevel::INFO, "DEPTH", "Surface zero=%.1fPa; sensor must be at the water surface",
               depthSensor.reading().surfacePressurePa);
  } else {
    logMessage(LogLevel::WARN, "DEPTH", "Surface zero failed: no valid pressure conversion");
  }
  recordSurfaceZero();
}
