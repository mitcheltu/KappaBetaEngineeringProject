#pragma once
#include "HistoryBuffer.h"
#include "SensorTelemetry.h"

namespace SensorHistory {
// Only existing v1 kinds 1..5 are emitted. No scenario events or model outputs.
template <typename Packet>
inline void append(History::Buffer& buffer, History::Kind kind, const Packet& packet,
                   uint64_t timeUs, uint8_t extraFlags = 0) {
  static_assert(sizeof(Packet) == 20, "History expects a v1 live payload");
  buffer.append(History::makeRecord(kind, packet.header.flags | extraFlags, timeUs,
      reinterpret_cast<const uint8_t*>(&packet) + sizeof(PacketHeader), 12));
}
inline void depth(History::Buffer& buffer, const SensorReadings& s, uint64_t timeUs, uint32_t now) {
  const auto packet = SensorTelemetry::depth(s, now);
  const float payload[4] = {packet.pressurePa, packet.depthM, packet.temperatureC, s.surfacePa};
  buffer.append(History::makeRecord(History::Kind::DEPTH, packet.header.flags, timeUs, payload, sizeof(payload)));
}
inline void imu(History::Buffer& buffer, const SensorReadings& s, uint64_t timeUs, uint32_t now) {
  append(buffer, History::Kind::IMU, SensorTelemetry::imu(s, now), timeUs);
}
inline void ppg(History::Buffer& buffer, const SensorReadings& s, uint64_t timeUs, uint32_t now) {
  append(buffer, History::Kind::PPG, SensorTelemetry::ppg(s, now), timeUs, 0x80);
}
inline void ppgGap(History::Buffer& buffer, uint64_t timeUs) {
  PpgPacket packet{};
  packet.hrTenths = packet.spo2Hundredths = UINT16_MAX;
  append(buffer, History::Kind::PPG, packet, timeUs);
}
inline void surfaceZero(History::Buffer& buffer, float surfacePa, uint64_t timeUs) {
  buffer.append(History::makeRecord(History::Kind::SURFACE_ZERO, std::isfinite(surfacePa) ? 1 : 0,
                                   timeUs, &surfacePa, sizeof(surfacePa)));
}
} // namespace SensorHistory
