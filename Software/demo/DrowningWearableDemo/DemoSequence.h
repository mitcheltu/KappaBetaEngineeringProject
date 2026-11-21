#pragma once
#include <stdint.h>

// Demo output sequence. The caller debounces SOS, calls update(), optionally
// offers an on-device inference trigger, then writes the motor/pump GPIOs.
//
// Two trigger sources share one sequence and one set of stop rules:
//   SOS        physical button press (release-to-rearm);
//   INFERENCE  on-device inference during a *scripted* scenario playback,
//              accepted only for the currently playing scenario run and at most
//              once per run. The web app can never call either trigger.
// Cancel, disabled outputs and the battery interlock stop either source.
class DemoSequence {
 public:
  enum class Stage : uint8_t { IDLE, VIBRATING, PUMPING };
  enum class Trigger : uint8_t { NONE = 0, SOS = 1, INFERENCE = 2 };
  static constexpr uint32_t VIBRATION_DURATION_MS = 10000;
  static constexpr uint32_t MOTOR_ON_MS = 1000;
  static constexpr uint32_t MOTOR_OFF_MS = 500;
  static constexpr uint32_t PUMP_DURATION_MS = 60000;

  Stage stage() const { return stage_; }
  Trigger trigger() const { return stage_ == Stage::IDLE ? Trigger::NONE : trigger_; }

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
      start(now, Trigger::SOS);
    }
  }

  // Offer an inference trigger. Call after update() in the same control tick.
  // requestedRun: the scenario run that raised the alert (nonzero).
  // playingRun:   the run the scenario controller reports as playing now.
  // Returns true only when the sequence started. A refused request is not
  // queued; the controller must not retry within the same run.
  bool requestInference(uint32_t now, bool enabled, bool cancel, bool batteryOk,
                        uint32_t requestedRun, uint32_t playingRun) {
    if (cancel || !enabled || !batteryOk) return false;
    if (stage_ != Stage::IDLE) return false;  // Never restarts or extends.
    if (requestedRun == 0 || requestedRun != playingRun) return false;
    if (requestedRun == lastInferenceRun_) return false;  // Once per run.
    lastInferenceRun_ = requestedRun;
    // A held SOS must still be released before it can start a later sequence.
    sosArmed_ = false;
    start(now, Trigger::INFERENCE);
    return true;
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
  void start(uint32_t now, Trigger source) {
    stage_ = Stage::VIBRATING;
    trigger_ = source;
    phaseStartedMs_ = now;
  }

  Stage stage_ = Stage::IDLE;
  Trigger trigger_ = Trigger::NONE;
  bool sosArmed_ = false;
  uint32_t phaseStartedMs_ = 0;
  uint32_t lastInferenceRun_ = 0;
};
