// Generate the checked-in cross-language fixture using the actual firmware types.
#include "HistoryTransfer.h"
#include "TelemetryProtocol.h"
#include <assert.h>
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

int main() {
  using namespace History;
  constexpr uint16_t token = 0xbeef;
  constexpr uint64_t firstTimeUs = 9007199254740993ULL; // Above JS Number's exact integer range.
  Record storage[8]{};
  Buffer buffer;
  buffer.reset(storage, 8, 100);
  const float depth[4] = {101800.5f, 0.25f, 22.5f, 101325.0f};
  const int16_t axes[6] = {-1000, 500, 8000, -20000, 20000, 1234};
  PpgPacket optical{};
  optical.red = 123456; optical.ir = 234567;
  optical.hrTenths = 723; optical.spo2Hundredths = 9876;
  SystemPacket system{};
  system.batteryMv = 3789; system.countdownRemainingMs = 9876;
  system.ppgOverflowCount = 0x12345678; system.sensorErrors = 0x90abcdef;
  const float baseline = 101325.0f;
  assert(buffer.append(makeRecord(Kind::DEPTH, 3, firstTimeUs, depth, sizeof(depth))));
  assert(buffer.append(makeRecord(Kind::IMU, 1, firstTimeUs + 40000, axes, sizeof(axes))));
  assert(buffer.append(makeRecord(Kind::PPG, 0x87, firstTimeUs + 80000, &optical.red, 12)));
  assert(buffer.append(makeRecord(Kind::SYSTEM, 0x73, firstTimeUs + 120000, &system.batteryMv, 12)));
  assert(buffer.append(makeRecord(Kind::SURFACE_ZERO, 1, firstTimeUs + 160000, &baseline, sizeof(baseline))));
  Record records[5]{};
  assert(buffer.copy(100, records, 5) == 5);
  Identity identity{};
  identity.flags = ALLOCATED | ML_RESERVED;
  const uint8_t mac[6] = {2, 0, 0, 0x12, 0x34, 0x56};
  memcpy(identity.deviceMac, mac, sizeof(mac));
  for (size_t i = 0; i < sizeof(identity.bootNonce); ++i) identity.bootNonce[i] = uint8_t(i + 1);
  Transfer transfer(buffer);
  transfer.setFlags(ALLOCATED | ML_RESERVED);
  assert(transfer.handle(Command{static_cast<uint8_t>(Opcode::OPEN), VERSION, token, 100}, 0));
  Status ready{}, block{};
  assert(transfer.nextStatus(ready));
  transfer.markStatusSent(0);
  assert(transfer.nextStatus(block) && block.type == static_cast<uint8_t>(StatusType::BLOCK));
  transfer.markStatusSent(0);
  std::vector<std::string> frames;
  uint8_t frame[20];
  for (uint32_t now = 1;; ++now) {
    const size_t length = transfer.nextFragment(frame, sizeof(frame));
    if (!length) break;
    frames.push_back(hex(frame, length));
    transfer.markFragmentSent(now);
  }
  const Command ack{static_cast<uint8_t>(Opcode::ACK), VERSION, token, 105};
  assert(block.detail == crc32(records, sizeof(records)));
  std::cout << "{\n  \"generator\": \"Software/tests/history_fixture.cpp\",\n"
            << "  \"identityHex\": \"" << hex(&identity, sizeof(identity)) << "\",\n"
            << "  \"recordBlockHex\": \"" << hex(records, sizeof(records)) << "\",\n"
            << "  \"statusHex\": \"" << hex(&block, sizeof(block)) << "\",\n"
            << "  \"commandHex\": \"" << hex(&ack, sizeof(ack)) << "\",\n"
            << "  \"crc32\": " << block.detail << ",\n  \"dataFragmentsHex\": [\n";
  for (size_t i = 0; i < frames.size(); ++i)
    std::cout << "    \"" << frames[i] << "\"" << (i + 1 < frames.size() ? "," : "") << "\n";
  std::cout << "  ],\n  \"expected\": {\n"
            << "    \"deviceId\": \"020000123456\", \"bootId\": \"0102030405060708\",\n"
            << "    \"first\": 100, \"end\": 105, \"token\": 48879,\n"
            << "    \"timeUs\": [\"9007199254740993\",\"9007199254780993\",\"9007199254820993\",\"9007199254860993\",\"9007199254900993\"],\n"
            << "    \"depth\": {\"pressurePa\":101800.5,\"depthM\":0.25,\"temperatureC\":22.5,\"surfacePa\":101325},\n"
            << "    \"imu\": {\"accelerationG\":[-1,0.5,8],\"angularVelocityDps\":[-2000,2000,123.4]},\n"
            << "    \"ppg\": {\"red\":123456,\"infrared\":234567,\"heartRateBpm\":72.3,\"spo2Percent\":98.76,\"estimatedSampleTime\":true},\n"
            << "    \"system\": {\"batteryV\":3.789,\"remainingMs\":9876,\"ppgOverflows\":305419896,\"sensorErrors\":2427178479},\n"
            << "    \"surfaceZeroPa\": 101325\n  }\n}\n";
}
