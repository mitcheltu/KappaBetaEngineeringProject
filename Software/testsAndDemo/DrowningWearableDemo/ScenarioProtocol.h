#pragma once
#include <cmath>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <type_traits>

// Demo scenario BLE protocol v1 (README.md, "Demo scenarios"). Little-endian.
// The app may only ARM a preloaded, scripted scenario or ABORT one that has not
// reached actuation. No command here starts, stops or times an output.
namespace Scenario {
constexpr uint8_t VERSION = 1;
constexpr uint8_t STATUS_TYPE = 1;

enum class Opcode : uint8_t { START = 1, ABORT = 2 };
enum class State : uint8_t {
  IDLE = 0, ARMED = 1, PLAYING = 2, ALERTED = 3, ACTUATING = 4,
  DONE = 5, ABORTED = 6, REJECTED = 7
};
enum class Reason : uint8_t {
  NONE = 0,               // Last command accepted / no reason.
  BATTERY_NOT_READY = 1,  // Battery outside the 3.4..4.35 V output interlock.
  SEQUENCE_ACTIVE = 2,    // A vibration/pump sequence is already running.
  SOS_HELD = 3,           // Physical SOS is held.
  UNKNOWN_SCENARIO = 4,   // Id not in the device catalog (always, for the Null source).
  CATALOG_MISMATCH = 5,   // App catalog CRC differs from the device catalog.
  ALREADY_RUNNING = 6,    // A scenario is ARMED/PLAYING/ALERTED/ACTUATING.
  NOT_RUNNING = 7,        // ABORT with nothing running.
  MALFORMED = 8,          // Reserved for diagnostics; malformed writes are dropped.
  SCENARIOS_DISABLED = 9,           // DEMO_SCENARIOS_ENABLED=0 or the control task is unavailable.
  CANCEL_HELD = 10,       // Physical Cancel (or Serial off) active.
  CANCELLED = 11,         // Run ended by physical Cancel / Serial off.
  APP_ABORT = 12,         // Run ended by an app ABORT before actuation.
  ACTUATING = 13,         // ABORT refused: outputs are running; use physical Cancel.
  TRIGGER_REFUSED = 14,   // The output sequence refused the inference trigger.
};
enum class Event : uint8_t {
  STARTED = 1, BUFFER_END = 2, PULSE_RESULT = 3, OXYGEN_RESULT = 4, ALERT = 5,
  ACTUATION_START = 6, ACTUATION_STOP = 7, CANCEL = 8, DONE = 9, REJECTED = 10,
  IMU_RESULT = 11
};
// Status flags (byte 7).
enum StatusFlag : uint8_t {
  PULSE_ALERT = 1, OXYGEN_ALERT = 2, TRIGGER_ACCEPTED = 4, PULSE_VALID = 8,
  CONCERN_VALID = 16, SCENARIOS_COMPILED = 32, PLAYBACK_FINISHED = 64,
  IMU_GATE_OPEN = 128
};
// Event flags (EventPayload::flags).
enum EventFlag : uint8_t {
  EV_ALERT = 1, EV_VALID = 2, EV_PULSE_TRACK = 4, EV_OXYGEN_TRACK = 8, EV_OVER_BUDGET = 16,
  EV_IMU_TRACK = 32
};

struct Command {
  uint8_t opcode = 0, version = VERSION;
  uint16_t token = 0;
  uint8_t scenarioId = 0, reserved = 0;
  uint16_t catalogCrc = 0;
};
struct Status {
  uint8_t type = STATUS_TYPE, version = VERSION;
  uint8_t state = 0, reason = 0;
  uint16_t commandToken = 0;
  uint8_t scenarioId = 0, flags = 0;
  uint32_t remainingMs = 0;
  uint16_t catalogCrc = 0;
  uint16_t pulseScore = UINT16_MAX, oxygenConcern = UINT16_MAX;
  uint16_t runToken = 0;
};
// One catalog entry. The catalog CRC is the low 16 bits of the IEEE CRC32 of
// all descriptors concatenated in catalog order (empty catalog -> 0x0000).
struct Descriptor {
  uint8_t id = 0, version = 0;
  uint16_t imuSamples = 0;  // 20 Hz IMU rows (6 axes each).
  uint32_t irSamples = 0, spo2Samples = 0;
  uint32_t contentCrc32 = 0;
};
// Catalog characteristic: header, then one entry per scenario.
struct CatalogHeader {
  uint8_t version = VERSION, count = 0;
  uint16_t catalogCrc = 0;
};
constexpr size_t NAME_BYTES = 24;  // NUL-padded UTF-8 display name.
struct CatalogEntry {
  Descriptor descriptor;
  char name[NAME_BYTES] = {};
};
static_assert(sizeof(CatalogHeader) == 4 && sizeof(CatalogEntry) == 40, "Catalog layout changed");
// History record kind 6 payload (16 bytes).
struct EventPayload {
  uint8_t event = 0, scenarioId = 0;
  uint16_t runToken = 0;
  uint32_t elapsedMs = 0;  // Since the START was accepted.
  float value = NAN;  // Pulse score / oxygen concern; NaN otherwise.
  uint16_t aux = UINT16_MAX;  // Pulse: analysis time /100 us; oxygen: SpO2 x100.
  uint8_t flags = 0;       // EventFlag bits; REJECTED/CANCEL: unused (see reason).
  uint8_t stateOrReason = 0;  // State after the event, or Reason for REJECTED/CANCEL.
};

static_assert(sizeof(Command) == 8 && offsetof(Command, token) == 2 &&
              offsetof(Command, scenarioId) == 4 && offsetof(Command, catalogCrc) == 6,
              "Scenario command layout changed");
static_assert(sizeof(Status) == 20 && offsetof(Status, commandToken) == 4 &&
              offsetof(Status, scenarioId) == 6 && offsetof(Status, remainingMs) == 8 &&
              offsetof(Status, catalogCrc) == 12 && offsetof(Status, pulseScore) == 14 &&
              offsetof(Status, oxygenConcern) == 16 && offsetof(Status, runToken) == 18,
              "Scenario status layout changed");
static_assert(sizeof(Descriptor) == 16, "Scenario descriptor layout changed");
static_assert(sizeof(EventPayload) == 16 && offsetof(EventPayload, elapsedMs) == 4 &&
              offsetof(EventPayload, value) == 8 && offsetof(EventPayload, aux) == 12 &&
              offsetof(EventPayload, flags) == 14, "Scenario event layout changed");
static_assert(std::is_trivially_copyable<EventPayload>::value, "Event must be copyable");

// An event plus the millis() time it happened (history record timestamp).
struct TimedEvent {
  uint32_t nowMs = 0;
  EventPayload payload;
};

inline uint32_t crc32Update(uint32_t crc, const void* data, size_t length) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
  }
  return crc;
}

// Structural validation only; state checks happen in the controller.
inline bool parseCommand(const void* bytes, size_t length, Command& out) {
  if (!bytes || length != sizeof(Command)) return false;
  Command candidate{};
  memcpy(&candidate, bytes, sizeof(candidate));
  if (candidate.version != VERSION || candidate.token == 0 || candidate.reserved != 0) return false;
  if (candidate.opcode != static_cast<uint8_t>(Opcode::START) &&
      candidate.opcode != static_cast<uint8_t>(Opcode::ABORT)) return false;
  out = candidate;
  return true;
}

// 0..1 scores as 1e-4 steps; NaN/unavailable is 0xFFFF.
inline uint16_t scaleUnit(float value) {
  if (!std::isfinite(value)) return UINT16_MAX;
  if (value <= 0.0f) return 0;
  if (value >= 1.0f) return 10000;
  return static_cast<uint16_t>(std::lround(value * 10000.0f));
}

inline bool running(State state) {
  return state == State::ARMED || state == State::PLAYING ||
         state == State::ALERTED || state == State::ACTUATING;
}
}  // namespace Scenario
