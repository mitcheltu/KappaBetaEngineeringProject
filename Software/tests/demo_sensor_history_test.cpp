// Test the demo's actual shared live/history encoders and emit a web fixture.
#include "SensorHistory.h"
#include "HistoryTransfer.h"
#include <cassert>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static std::string hex(const void* data, size_t length) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (size_t i = 0; i < length; ++i) out << std::setw(2) << unsigned(bytes[i]);
  return out.str();
}

int main(int argc, char**) {
  using namespace History;
  constexpr uint32_t now = 5000;
  constexpr uint64_t timeUs = 9007199254740993ULL;
  SensorReadings s;
  s.pressureValid = s.imuValid = s.rawPpgValid = s.hrValid = s.spo2Valid = true;
  s.pressureMs = s.imuMs = s.ppgMs = s.estimateMs = now;
  s.pressurePa = 103769.3046875f; s.surfacePa = 101325; s.temperatureC = 22.5f;
  const float axes[] = {-1, 0.5f, 8, -2000, 2000, 123.4f};
  memcpy(s.imuAxes, axes, sizeof(axes));
  s.red = 123456; s.ir = 234567; s.heartRate = 72; s.oxygenSaturation = 98;
  s.ppgOverflows = 2; s.sensorErrors = 3;
  auto depth = SensorTelemetry::depth(s, now);
  auto imu = SensorTelemetry::imu(s, now);
  auto ppg = SensorTelemetry::ppg(s, now);
  auto system = SensorTelemetry::system(s, 3789, (9876u << 8) | 32 | 64, true);
  assert(depth.header.flags == 3 && std::abs(depth.depthM - 0.25f) < 0.00001f);
  assert(imu.axes[0] == -1000 && imu.axes[3] == -20000 && imu.axes[5] == 1234);
  assert(ppg.header.flags == 7 && ppg.hrTenths == 720 && ppg.spo2Hundredths == 9800);
  assert(system.header.flags == 0xe3 && system.countdownRemainingMs == 9876);

  // Exact freshness boundaries and millis rollover must match live and history.
  assert(SensorTelemetry::depth(s, now + 999).header.flags == 3);
  assert(SensorTelemetry::depth(s, now + 1000).header.flags == 0);
  assert(SensorTelemetry::imu(s, now + 299).header.flags == 1);
  assert(SensorTelemetry::imu(s, now + 300).header.flags == 0);
  assert(SensorTelemetry::ppg(s, now + 300).header.flags == 0);
  SensorReadings staleEstimate = s;
  staleEstimate.estimateMs = now - 2000;
  assert(SensorTelemetry::ppg(staleEstimate, now).header.flags == 1);
  assert(SensorTelemetry::ppg(staleEstimate, now).hrTenths == UINT16_MAX);
  SensorReadings rollover = s;
  rollover.pressureMs = rollover.imuMs = rollover.ppgMs = rollover.estimateMs = UINT32_MAX - 99;
  assert(SensorTelemetry::imu(rollover, 100).header.flags == 1);
  assert(SensorTelemetry::ppg(rollover, 100).header.flags == 7);
  assert(SensorTelemetry::imu(rollover, 200).header.flags == 0);

  Record storage[16]{};
  Buffer buffer;
  buffer.reset(storage, 16, 100);
  SensorHistory::depth(buffer, s, timeUs, now);
  SensorHistory::imu(buffer, s, timeUs + 1, now);
  SensorHistory::ppg(buffer, s, timeUs + 2, now);
  SensorHistory::append(buffer, Kind::SYSTEM, system, timeUs + 3);
  SensorHistory::surfaceZero(buffer, s.surfacePa, timeUs + 4);
  s.hrValid = s.spo2Valid = false;
  SensorHistory::ppg(buffer, s, timeUs + 5, now);
  SensorHistory::ppgGap(buffer, timeUs + 6);
  s.pressureValid = false; s.surfacePa = NAN;
  SensorHistory::depth(buffer, s, timeUs + 7, now);
  SensorHistory::imu(buffer, s, timeUs + 8, now + 300);
  SensorHistory::surfaceZero(buffer, s.surfacePa, timeUs + 9);
  SensorHistory::append(buffer, Kind::SYSTEM,
      SensorTelemetry::system(s, UINT16_MAX, 0, false), timeUs + 10);
  Record records[11]{};
  assert(buffer.copy(100, records, 11) == 11);
  for (size_t i = 0; i < 11; ++i) {
    assert(records[i].sequence == 100 + i && records[i].kind >= 1 && records[i].kind <= 5);
    assert(records[i].timestampUs == timeUs + i && records[i].reserved == 0);
  }
  assert(memcmp(records[0].payload, reinterpret_cast<const uint8_t*>(&depth) + 8, 12) == 0);
  assert(memcmp(records[1].payload, reinterpret_cast<const uint8_t*>(&imu) + 8, 12) == 0);
  assert(memcmp(records[2].payload, reinterpret_cast<const uint8_t*>(&ppg) + 8, 12) == 0);
  assert(records[2].flags == 0x87 && records[5].flags == 0x81 && records[6].flags == 0);
  assert(records[7].flags == 0 && records[8].flags == 0 && records[9].flags == 0);

  Transfer transfer(buffer);
  transfer.setFlags(ALLOCATED); // Demo has history but does not reserve ML memory.
  assert(transfer.handle(Command{uint8_t(Opcode::OPEN), VERSION, 42, 100}, 0));
  Status ready{}, block{};
  assert(transfer.nextStatus(ready) && ready.detail == 16 && ready.flags == ALLOCATED);
  transfer.markStatusSent(0);
  assert(transfer.nextStatus(block) && block.type == uint8_t(StatusType::BLOCK));
  transfer.markStatusSent(0);
  // Sampling continues while the immutable preconnection snapshot is sent.
  SensorHistory::ppgGap(buffer, timeUs + 11);
  std::vector<std::string> fragments;
  uint8_t frame[20]; // Default ATT MTU, including split records.
  while (const auto length = transfer.nextFragment(frame, sizeof(frame))) {
    fragments.push_back(hex(frame, length));
    transfer.markFragmentSent(1);
  }
  assert(transfer.handle(Command{uint8_t(Opcode::ACK), VERSION, 42, 111}, 2));
  Status end{};
  assert(transfer.nextStatus(end) && end.type == uint8_t(StatusType::END) && end.end == 111);
  transfer.markStatusSent(2);
  assert(transfer.handle(Command{uint8_t(Opcode::LIVE), VERSION, 42, 111}, 3));
  Status live{};
  assert(transfer.nextStatus(live) && live.type == uint8_t(StatusType::LIVE));
  transfer.markStatusSent(3);
  assert(transfer.liveEnabled());
  assert(transfer.handle(Command{uint8_t(Opcode::POLL), VERSION, 43, 111}, 4));
  assert(transfer.snapshotEnd() == 112 && transfer.liveEnabled());
  transfer.disconnect();
  assert(!transfer.liveEnabled() && buffer.next() == 112);
  assert(transfer.handle(Command{uint8_t(Opcode::OPEN), VERSION, 44, 111}, 5));
  assert(transfer.cursor() == 111 && transfer.snapshotEnd() == 112);

  if (argc == 1) {
    std::cout << "Demo live/history encoders, validity, timestamps, snapshot, polling and resume passed\n";
    return 0;
  }
  Identity identity{};
  identity.flags = ALLOCATED;
  identity.deviceMac[0] = 2; identity.bootNonce[0] = 1;
  std::cout << "{\n  \"generator\": \"Software/tests/demo_sensor_history_test.cpp\",\n"
      << "  \"identityHex\": \"" << hex(&identity, sizeof(identity)) << "\",\n"
      << "  \"live\": {\"depth\":\"" << hex(&depth, 20) << "\",\"imu\":\"" << hex(&imu, 20)
      << "\",\"ppg\":\"" << hex(&ppg, 20) << "\",\"system\":\"" << hex(&system, 20) << "\"},\n"
      << "  \"recordBlockHex\": \"" << hex(records, sizeof(records)) << "\",\n"
      << "  \"statusHex\": \"" << hex(&block, sizeof(block)) << "\",\n"
      << "  \"dataFragmentsHex\": [\n";
  for (size_t i = 0; i < fragments.size(); ++i)
    std::cout << "    \"" << fragments[i] << "\"" << (i + 1 < fragments.size() ? "," : "") << '\n';
  std::cout << "  ]\n}\n";
}
