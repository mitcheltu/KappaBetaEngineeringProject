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

void serviceDepth() {
  const DepthServiceEvent event = depthSensor.service();
  if (event == DepthServiceEvent::NONE) return;

  if (event == DepthServiceEvent::SAMPLE_PUBLISHED) {
    sensorBuffers.pushDepth(depthSensor.reading());
    recordDepthSample();
    return;
  }

  if (event == DepthServiceEvent::SURFACE_ZERO_PUBLISHED) {
    sensorBuffers.pushDepth(depthSensor.reading());
    logMessage(LogLevel::INFO, "DEPTH",
               "Surface zero=%.1fPa; sensor must be at the water surface",
               depthSensor.reading().surfacePressurePa);
    recordSurfaceZero();
    return;
  }

  const DepthReading& reading = depthSensor.reading();
  sensorBuffers.pushDepth(reading);
  logMessage(LogLevel::WARN,
             "DEPTH",
             "MS5837 %s: pressure=%.1fPa temperature=%.2fC quality=0x%04x",
             event == DepthServiceEvent::SURFACE_ZERO_FAILED ? "surface zero failed"
                                                              : "sample invalid",
             reading.pressurePa, reading.temperatureC, unsigned(reading.qualityFlags));
  ++sensorErrors;
  depthReady = false;
  if (event == DepthServiceEvent::SURFACE_ZERO_FAILED) recordSurfaceZero();
}

void serviceDepthZero() {
  if (!zeroRequested.exchange(false)) return;
  if (depthReady && depthSensor.startDive()) {
    logMessage(LogLevel::INFO, "DEPTH",
               "Surface zero requested; fresh nonblocking conversion queued");
    return;
  }

  logMessage(LogLevel::WARN, "DEPTH",
             "Surface zero failed: depth sensor is not ready");
  recordSurfaceZero();
}
