#pragma once
#include <stdint.h>
#include "FirmwareConfig.h"

enum class LogLevel : uint8_t { ERROR = 0, WARN = 1, INFO = 2, DEBUG = 3 };
enum class BatteryCondition : uint8_t { UNKNOWN, INVALID_LOW, LOW_BATTERY, READY, INVALID_HIGH };

inline BatteryCondition batteryCondition(uint32_t mv) {
  if (mv == UINT16_MAX) return BatteryCondition::UNKNOWN;
  if (mv < Config::BATTERY_VALID_MIN_MV) return BatteryCondition::INVALID_LOW;
  if (mv < Config::ACTUATOR_MIN_BATTERY_MV) return BatteryCondition::LOW_BATTERY;
  if (mv <= Config::BATTERY_VALID_MAX_MV) return BatteryCondition::READY;
  return BatteryCondition::INVALID_HIGH;
}

inline const char* batteryConditionName(BatteryCondition state) {
  switch (state) {
    case BatteryCondition::INVALID_LOW: return "INVALID_LOW";
    case BatteryCondition::LOW_BATTERY: return "LOW";
    case BatteryCondition::READY: return "READY";
    case BatteryCondition::INVALID_HIGH: return "INVALID_HIGH";
    default: return "NOT_SAMPLED";
  }
}

bool initializeLogging();
void logMessage(LogLevel level, const char* tag, const char* format, ...)
    __attribute__((format(printf, 3, 4)));
void setLogLevel(int level); // -1 disables; status/help still explicitly reply.
void logReply(const char* tag, const char* format, ...) __attribute__((format(printf, 2, 3)));
void logStatus();
void serviceDiagnostics(uint32_t now);
