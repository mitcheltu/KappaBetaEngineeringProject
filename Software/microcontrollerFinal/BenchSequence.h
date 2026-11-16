#pragma once
#include <stdint.h>
#include "FirmwareConfig.h"

// Manual sequence state only. GPIO writes belong to the caller's control task.
class BenchSequence {
 public:
  enum class Stage : uint8_t { IDLE, VIBRATING, PUMP_RAMP, PUMP_RUNNING };
  Stage stage = Stage::IDLE;

  void update(uint32_t now, bool enabled, bool cancel, bool sosPressed,
              bool batteryOk) {
    // Cancel has priority at every phase boundary. An aborted sequence never
    // resumes, and a held SOS cannot restart it when Cancel is released.
    if (cancel || !enabled || !batteryOk) {
      stage = Stage::IDLE;
      sosArmed = false;
      return;
    }

    if (stage == Stage::VIBRATING) {
      sosArmed = false;
      if (now - phaseStartedMs >= Config::VIBRATION_DURATION_MS) {
        stage = Stage::PUMP_RAMP;
        phaseStartedMs = now;
      }
      return;
    }
    if (stage == Stage::PUMP_RAMP) {
      sosArmed = false;
      if (now - phaseStartedMs >= Config::PUMP_RAMP_DURATION_MS) {
        stage = Stage::PUMP_RUNNING;
        phaseStartedMs = now;
      }
      return;
    }
    if (stage == Stage::PUMP_RUNNING) {
      sosArmed = false;
      return;
    }

    // Observe release while idle before accepting a press. This prevents a held
    // reed at boot, a press during a sequence, or a held press after abort from
    // starting/restarting the pump. Releasing SOS does not cancel a sequence.
    if (!sosPressed) {
      sosArmed = true;
      return;
    }
    if (sosArmed) {
      sosArmed = false;
      stage = Stage::VIBRATING;
      phaseStartedMs = now;
    }
  }

  void abort() {
    stage = Stage::IDLE;
    sosArmed = false;
  }

  bool startAutomatically(uint32_t now, bool enabled, bool cancel,
                          bool batteryOk) {
    if (stage != Stage::IDLE || !enabled || cancel || !batteryOk) return false;
    stage = Stage::VIBRATING;
    phaseStartedMs = now;
    sosArmed = false;
    return true;
  }

  bool vibrationOn() const { return stage == Stage::VIBRATING; }
  bool pumpOn() const {
    return stage == Stage::PUMP_RAMP || stage == Stage::PUMP_RUNNING;
  }
  bool canAcceptSos() const { return stage == Stage::IDLE && sosArmed; }

  uint32_t remainingVibrationMs(uint32_t now) const {
    if (stage != Stage::VIBRATING) return 0;
    const uint32_t elapsed = now - phaseStartedMs;
    return elapsed < Config::VIBRATION_DURATION_MS
        ? Config::VIBRATION_DURATION_MS - elapsed : 0;
  }

  uint32_t pumpDuty(uint32_t now) const {
    if (stage == Stage::PUMP_RUNNING) return Config::PUMP_PWM_MAX_DUTY;
    if (stage != Stage::PUMP_RAMP) return 0;
    const uint32_t elapsed = now - phaseStartedMs;
    const uint32_t rampElapsed = elapsed < Config::PUMP_RAMP_DURATION_MS
        ? elapsed : Config::PUMP_RAMP_DURATION_MS;
    return (Config::PUMP_PWM_MAX_DUTY * rampElapsed) /
        Config::PUMP_RAMP_DURATION_MS;
  }

  static const char* stageName(Stage value) {
    switch (value) {
      case Stage::IDLE: return "IDLE";
      case Stage::VIBRATING: return "VIBRATING";
      case Stage::PUMP_RAMP: return "PUMP_RAMP";
      case Stage::PUMP_RUNNING: return "PUMP_RUNNING";
    }
    return "UNKNOWN";
  }

 private:
  bool sosArmed = false;
  uint32_t phaseStartedMs = 0;
};
