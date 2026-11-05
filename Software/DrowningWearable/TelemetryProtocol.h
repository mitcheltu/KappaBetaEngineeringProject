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
static_assert(sizeof(float) == 4 && sizeof(PacketHeader) == 8, "Unexpected wire representation");
static_assert(sizeof(DepthPacket) == 20 && sizeof(ImuPacket) == 20 &&
              sizeof(PpgPacket) == 20 && sizeof(SystemPacket) == 20, "BLE packet size changed");
static_assert(offsetof(PpgPacket, hrTenths) == 16 && offsetof(SystemPacket, sensorErrors) == 16,
              "BLE packet offsets changed");
