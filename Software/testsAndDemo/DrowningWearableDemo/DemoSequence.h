#pragma once
#include <stdint.h>

// Physical-button demo sequence. The caller debounces SOS, calls update(),
// then writes the motor/pump GPIOs from the resulting output state.
class DemoSequence {
 public:
  enum class Stage : uint8_t { IDLE, VIBRATING, PUMPING };
  static constexpr uint32_t VIBRATION_DURATION_MS = 10000;
  static constexpr uint32_t MOTOR_ON_MS = 1000;
  static constexpr uint32_t MOTOR_OFF_MS = 500;
  static constexpr uint32_t PUMP_DURATION_MS = 60000;

  Stage stage() const { return stage_; }

  void update(uint32_t now, bool enabled, bool cancel, bool sosPressed,
              bool batteryOk) {
    // Stops take priority even at a phase deadline or simultaneous SOS press.
    // Recovery always requires a release observed while idle.
    if (cancel || !enabled || !batteryOk) {
      stage_ = Stage::IDLE;
      sosArmed_ = false;
      return;
    }

    if (stage_ == Stage::VIBRATING) {
      sosArmed_ = false;
      if (now - phaseStartedMs_ >= VIBRATION_DURATION_MS) {
        stage_ = Stage::PUMPING;
        // Give the pump a full 60 seconds from its actual activation, even
        // when this update runs after the vibration deadline.
        phaseStartedMs_ = now;
      }
      return;
    }

    if (stage_ == Stage::PUMPING) {
      sosArmed_ = false;
      if (now - phaseStartedMs_ >= PUMP_DURATION_MS) stage_ = Stage::IDLE;
      return;
    }

    // A held SOS at boot, during a sequence, or after an abort cannot trigger
    // a sequence. Presses during an active phase are never queued.
    if (!sosPressed) {
      sosArmed_ = true;
    } else if (sosArmed_) {
      sosArmed_ = false;
      stage_ = Stage::VIBRATING;
      phaseStartedMs_ = now;
    }
  }

  bool motorOn(uint32_t now) const {
    if (stage_ != Stage::VIBRATING) return false;
    const uint32_t elapsed = now - phaseStartedMs_;
    return elapsed < VIBRATION_DURATION_MS &&
           elapsed % (MOTOR_ON_MS + MOTOR_OFF_MS) < MOTOR_ON_MS;
  }

  bool pumpOn() const { return stage_ == Stage::PUMPING; }

  uint32_t remainingPhaseMs(uint32_t now) const {
    if (stage_ == Stage::IDLE) return 0;
    const uint32_t duration = stage_ == Stage::VIBRATING
        ? VIBRATION_DURATION_MS : PUMP_DURATION_MS;
    const uint32_t elapsed = now - phaseStartedMs_;
    return elapsed < duration ? duration - elapsed : 0;
  }

  bool canAcceptSos() const { return stage_ == Stage::IDLE && sosArmed_; }

 private:
  Stage stage_ = Stage::IDLE;
  bool sosArmed_ = false;
  uint32_t phaseStartedMs_ = 0;
};
