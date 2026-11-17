#pragma once
#include "FirmwareConfig.h"

// Nonblocking linear start. A stop resets the ramp; elapsed time wraps safely.
class PumpRamp {
 public:
  uint32_t update(uint32_t now, bool requested) {
    if (!requested) { active_ = false; return 0; }
    if (!active_) { active_ = true; startedMs_ = now; }
    const uint32_t elapsed = now - startedMs_;
    if (elapsed >= Config::PUMP_RAMP_MS) return Config::PUMP_MAX_DUTY;
    return uint64_t(Config::PUMP_MAX_DUTY) * elapsed / Config::PUMP_RAMP_MS;
  }
 private:
  bool active_ = false;
  uint32_t startedMs_ = 0;
};
