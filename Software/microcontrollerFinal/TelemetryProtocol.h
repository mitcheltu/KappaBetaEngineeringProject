#pragma once
#include <stdint.h>
#include <stddef.h>

// ESP32 little-endian, 20 bytes each; fits the default BLE ATT MTU.
struct PacketHeader {
  uint8_t version = 1, flags = 0;
  uint16_t sequence = 0;
  uint32_t uptimeMs = 0;
};
struct DepthPacket { PacketHeader header; float pressurePa, depthM, temperatureC; };
struct ImuPacket { PacketHeader header; int16_t axes[6]; };
struct PpgPacket { PacketHeader header; uint32_t red, ir; uint16_t hrTenths, spo2Hundredths; };
struct SystemPacket {
  PacketHeader header;
  uint16_t batteryMv, countdownRemainingMs;
  uint32_t ppgOverflowCount, sensorErrors;
};

// Status bits belong to RiskPacket::statusFlags (the header's flags retain
// the standard telemetry meaning). Timestamps are microseconds since boot.
namespace RiskPacketFlags {
constexpr uint32_t RESULT_AVAILABLE = 1u << 0;
constexpr uint32_t RESULT_FRESH = 1u << 1;
constexpr uint32_t NEW_RESULT = 1u << 2;
constexpr uint32_t GATE_OPEN = 1u << 3;
constexpr uint32_t CNN_VALID = 1u << 4;
constexpr uint32_t DEPTH_VALID = 1u << 5;
constexpr uint32_t PPG_VALID = 1u << 6;
constexpr uint32_t FUSION_COMPLETE = 1u << 7;
constexpr uint32_t FUSION_VALID = 1u << 8;
constexpr uint32_t PPG_DUMMY_USED = 1u << 9;
} // namespace RiskPacketFlags

struct RiskPacket {
  PacketHeader header;
  uint32_t roundId;
  uint32_t statusFlags;
  uint64_t targetUs;
  uint64_t completedAtUs;
  uint64_t cnnStartUs, cnnEndUs;
  uint64_t depthStartUs, depthEndUs;
  uint64_t ppgStartUs, ppgEndUs;
  float cnnScore, depthRiskScore, ppgRiskScore, combinedRisk;
};
static_assert(sizeof(float) == 4 && sizeof(PacketHeader) == 8, "Unexpected wire representation");
static_assert(sizeof(DepthPacket) == 20 && sizeof(ImuPacket) == 20 &&
              sizeof(PpgPacket) == 20 && sizeof(SystemPacket) == 20, "BLE packet size changed");
static_assert(sizeof(RiskPacket) == 96 && offsetof(RiskPacket, targetUs) == 16 &&
              offsetof(RiskPacket, cnnScore) == 80,
              "Risk BLE packet wire layout changed");
static_assert(offsetof(PpgPacket, hrTenths) == 16 && offsetof(SystemPacket, sensorErrors) == 16,
              "BLE packet offsets changed");
