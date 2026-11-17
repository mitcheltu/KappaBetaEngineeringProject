#pragma once
#include <cmath>
#include "FirmwareConfig.h"
#include "TelemetryProtocol.h"

// Loop-owned measurements; shared encoders keep live and saved values identical.
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

namespace SensorTelemetry {
inline DepthPacket depth(const SensorReadings& s, uint32_t now) {
  DepthPacket packet{};
  const bool fresh = s.pressureValid && uint32_t(now - s.pressureMs) < 1000;
  const bool zeroed = fresh && std::isfinite(s.surfacePa);
  packet.header.flags = (fresh ? 1 : 0) | (zeroed ? 2 : 0);
  packet.pressurePa = fresh ? s.pressurePa : NAN;
  packet.temperatureC = fresh ? s.temperatureC : NAN;
  packet.depthM = zeroed ? (s.pressurePa - s.surfacePa) / (Config::FLUID_DENSITY_KG_M3 * 9.80665f) : NAN;
  return packet;
}
inline ImuPacket imu(const SensorReadings& s, uint32_t now) {
  ImuPacket packet{};
  if (s.imuValid && uint32_t(now - s.imuMs) < 300) {
    packet.header.flags = 1;
    for (size_t i = 0; i < 6; ++i)
      packet.axes[i] = lroundf(s.imuAxes[i] * (i < 3 ? 1000.0f : 10.0f));
  }
  return packet;
}
inline PpgPacket ppg(const SensorReadings& s, uint32_t now) {
  PpgPacket packet{};
  packet.hrTenths = packet.spo2Hundredths = UINT16_MAX;
  if (s.rawPpgValid && uint32_t(now - s.ppgMs) < 300) {
    packet.header.flags = 1;
    packet.red = s.red;
    packet.ir = s.ir;
    if (uint32_t(now - s.estimateMs) < 2000) {
      if (s.hrValid) { packet.header.flags |= 2; packet.hrTenths = s.heartRate * 10; }
      if (s.spo2Valid) { packet.header.flags |= 4; packet.spo2Hundredths = s.oxygenSaturation * 100; }
    }
  }
  return packet;
}
inline SystemPacket system(const SensorReadings& s, uint16_t battery,
                           uint32_t controls, bool enabled) {
  SystemPacket packet{};
  packet.header.flags = 0x80 | (battery != UINT16_MAX ? 1 : 0) |
      (enabled ? 2 : 0) | static_cast<uint8_t>(controls);
  packet.batteryMv = battery;
  packet.countdownRemainingMs = static_cast<uint16_t>(controls >> 8);
  packet.ppgOverflowCount = s.ppgOverflows;
  packet.sensorErrors = s.sensorErrors;
  return packet;
}
} // namespace SensorTelemetry
