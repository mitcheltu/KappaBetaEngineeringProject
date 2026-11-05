#pragma once
#include <stdint.h>

// Manual bench sequence only. GPIO writes belong to the caller's control task.
class BenchSequence {
 public:
  enum class Stage : uint8_t { IDLE, COUNTDOWN, RUNNING };
  static constexpr uint32_t COUNTDOWN_MS = 10000;
  Stage stage = Stage::IDLE;

  // runDurationMs == 0 holds RUNNING until Cancel/disable/low battery.
  // A positive duration is captured when a new sequence starts.
  void update(uint32_t now, bool enabled, bool cancel, bool sosPressed,
              bool batteryOk, uint32_t runDurationMs) {
    // Cancel has priority even at either transition deadline. Aborted sequences
    // never resume, and an already-held SOS cannot restart one when Cancel clears.
    if (cancel || !enabled || !batteryOk) {
      stage = Stage::IDLE;
      sosArmed = false;
      return;
    }

    if (stage == Stage::COUNTDOWN) {
      sosArmed = false;
      if (now - phaseStartedMs >= COUNTDOWN_MS) {
        stage = Stage::RUNNING;
        phaseStartedMs = now;
      }
      return;
    }
    if (stage == Stage::RUNNING) {
      sosArmed = false;
      if (runForMs != 0 && now - phaseStartedMs >= runForMs)
        stage = Stage::IDLE;
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
      stage = Stage::COUNTDOWN;
      phaseStartedMs = now;
      runForMs = runDurationMs;
    }
  }

  bool vibrationOn() const { return stage == Stage::COUNTDOWN; }
  bool pumpOn() const { return stage == Stage::RUNNING; }

  uint32_t remainingCountdownMs(uint32_t now) const {
    if (stage != Stage::COUNTDOWN) return 0;
    const uint32_t elapsed = now - phaseStartedMs;
    return elapsed < COUNTDOWN_MS ? COUNTDOWN_MS - elapsed : 0;
  }

  // Zero means no timed stop when RUNNING with runDurationMs == 0; otherwise
  // zero means the phase is inactive or its deadline has elapsed.
  uint32_t remainingRunMs(uint32_t now) const {
    if (stage != Stage::RUNNING || runForMs == 0) return 0;
    const uint32_t elapsed = now - phaseStartedMs;
    return elapsed < runForMs ? runForMs - elapsed : 0;
  }

 private:
  bool sosArmed = false;
  uint32_t phaseStartedMs = 0;
  uint32_t runForMs = 0;
};
