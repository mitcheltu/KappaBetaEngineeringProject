#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <type_traits>

// Sensor history protocol v1. ESP32 and the host tests are little-endian.
namespace History {
constexpr uint8_t VERSION = 1;
constexpr size_t RECORD_BYTES = 32;
constexpr size_t MAX_BLOCK_RECORDS = 64;
constexpr size_t MAX_ATT_BYTES = 244;

enum class Kind : uint8_t { DEPTH = 1, IMU = 2, PPG = 3, SYSTEM = 4, SURFACE_ZERO = 5 };
enum class Opcode : uint8_t { OPEN = 1, ACK = 2, LIVE = 3, POLL = 4, ABORT = 5 };
enum class StatusType : uint8_t { READY = 1, BLOCK = 2, END = 3, GAP = 4, LIVE = 5, ERROR = 6 };
enum class Error : uint32_t { BAD_CURSOR = 1, TIMEOUT = 2, INVALID_STATE = 3 };
enum Flag : uint16_t { ALLOCATED = 1, ML_RESERVED = 2, SEQUENCE_EXHAUSTED = 4 };

struct Record {
  uint32_t sequence = 0;
  uint8_t kind = 0, flags = 0;
  uint16_t reserved = 0;
  uint64_t timestampUs = 0;
  uint8_t payload[16] = {};
};
struct Identity {
  uint8_t version = VERSION, recordSize = RECORD_BYTES, flags = 0, reserved = 0;
  uint8_t deviceMac[6] = {}, bootNonce[8] = {}, trailingReserved[2] = {};
};
struct Command {
  uint8_t opcode = 0, version = VERSION;
  uint16_t token = 0;
  uint32_t cursor = 0;
};
struct Status {
  uint8_t type = 0, version = VERSION;
  uint16_t token = 0;
  uint32_t first = 0, end = 0, detail = 0;
  uint16_t count = 0, flags = 0;
};
struct FragmentHeader {
  uint16_t token = 0, offset = 0;
  uint32_t first = 0;
};

static_assert(sizeof(Record) == 32 && offsetof(Record, kind) == 4 &&
              offsetof(Record, flags) == 5 && offsetof(Record, reserved) == 6 &&
              offsetof(Record, timestampUs) == 8 && offsetof(Record, payload) == 16,
              "History record layout changed");
static_assert(sizeof(Identity) == 20 && offsetof(Identity, deviceMac) == 4 &&
              offsetof(Identity, bootNonce) == 10, "History identity layout changed");
static_assert(sizeof(Command) == 8 && offsetof(Command, token) == 2 &&
              offsetof(Command, cursor) == 4, "History command layout changed");
static_assert(sizeof(Status) == 20 && offsetof(Status, token) == 2 &&
              offsetof(Status, first) == 4 && offsetof(Status, end) == 8 &&
              offsetof(Status, detail) == 12 && offsetof(Status, count) == 16 &&
              offsetof(Status, flags) == 18, "History status layout changed");
static_assert(sizeof(FragmentHeader) == 8 && offsetof(FragmentHeader, first) == 4,
              "History fragment layout changed");
static_assert(std::is_trivially_copyable<Record>::value, "History record must be copyable");

inline uint32_t crc32(const void* data, size_t length) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
  }
  return crc ^ 0xffffffffu;
}

inline bool validCommand(const Command& command) {
  return command.version == VERSION && command.token != 0 &&
         command.opcode >= static_cast<uint8_t>(Opcode::OPEN) &&
         command.opcode <= static_cast<uint8_t>(Opcode::ABORT);
}

inline bool parseCommand(const void* bytes, size_t length, Command& out) {
  if (!bytes || length != sizeof(Command)) return false;
  Command candidate{};
  memcpy(&candidate, bytes, sizeof(candidate));
  if (!validCommand(candidate)) return false;
  out = candidate;
  return true;
}

inline Record makeRecord(Kind kind, uint8_t flags, uint64_t timestampUs,
                         const void* payload = nullptr, size_t payloadBytes = 0) {
  Record record{};
  record.kind = static_cast<uint8_t>(kind);
  record.flags = flags;
  record.timestampUs = timestampUs;
  if (payload && payloadBytes <= sizeof(record.payload)) memcpy(record.payload, payload, payloadBytes);
  return record;
}
} // namespace History
