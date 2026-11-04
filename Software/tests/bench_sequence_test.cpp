#include "BenchSequence.h"
#include <cassert>
#include <iostream>

using Stage = BenchSequence::Stage;
void off(const BenchSequence& sequence) {
  assert(sequence.stage == Stage::IDLE);
  assert(!sequence.vibrationOn() && !sequence.valveOn());
}
void press(BenchSequence& sequence, uint32_t now, uint32_t duration = 0) {
  sequence.update(now - 1, true, false, false, true, duration);
  sequence.update(now, true, false, true, true, duration);
  assert(sequence.stage == Stage::COUNTDOWN);
  assert(sequence.vibrationOn() && !sequence.valveOn());
}
int main() {
  {
    BenchSequence sequence;
    // A reed held at boot must never start, regardless of how long it is held.
    sequence.update(0, true, false, true, true, 0);
    sequence.update(100000, true, false, true, true, 0);
    off(sequence);
    press(sequence, 100010);
    assert(sequence.remainingCountdownMs(100010) == 10000);
    sequence.update(110009, true, false, false, true, 0);
    assert(sequence.remainingCountdownMs(110009) == 1);
    assert(sequence.vibrationOn() && !sequence.valveOn());
    sequence.update(110010, true, false, false, true, 0);
    assert(sequence.stage == Stage::OPEN);
    assert(!sequence.vibrationOn() && sequence.valveOn());
    assert(sequence.remainingCountdownMs(110010) == 0);
    assert(sequence.remainingOpenMs(110010) == 0);
    // Releasing SOS does not cancel; zero duration really is hold-until-Cancel.
    sequence.update(2000000, true, false, false, true, 0);
    assert(sequence.valveOn());
    sequence.update(2000001, true, true, false, true, 0);
    off(sequence);
  }
  // Cancel at start, during countdown, exactly at opening, and after opening.
  for (uint32_t elapsed : {0u, 123u, 9999u, 10000u, 10001u}) {
    BenchSequence sequence;
    press(sequence, 100);
    if (elapsed > 10000) sequence.update(10100, true, false, true, true, 0);
    sequence.update(100 + elapsed, true, true, true, true, 0);
    off(sequence);
    sequence.update(20000, true, false, true, true, 0);
    off(sequence); // The old held SOS cannot rearm after Cancel.
    press(sequence, 20002);
  }
  {
    BenchSequence sequence;
    sequence.update(0, true, false, false, true, 0);
    sequence.update(1, true, true, true, true, 0); // Simultaneous buttons.
    off(sequence);
    sequence.update(2, true, false, true, true, 0);
    off(sequence);
  }
  // Either interlock aborts either active phase and never resumes on recovery.
  for (bool duringOpen : {false, true}) {
    for (bool disabled : {false, true}) {
      BenchSequence sequence;
      press(sequence, 10);
      if (duringOpen) sequence.update(10010, true, false, true, true, 0);
      const uint32_t now = duringOpen ? 10011 : 11;
      sequence.update(now, !disabled, false, true, disabled, 0);
      off(sequence);
      sequence.update(now + 1, true, false, true, true, 0);
      off(sequence);
      press(sequence, now + 3);
    }
  }
  {
    BenchSequence sequence;
    press(sequence, 50, 250);
    // Mid-countdown presses must not reset the 10-second deadline.
    sequence.update(4000, true, false, false, true, 999);
    sequence.update(4001, true, false, true, true, 999);
    sequence.update(10050, true, false, false, true, 999);
    assert(sequence.valveOn());
    assert(sequence.remainingOpenMs(10050) == 250); // Duration latched on SOS.
    sequence.update(10299, true, false, true, true, 999);
    assert(sequence.remainingOpenMs(10299) == 1);
    sequence.update(10300, true, false, true, true, 999);
    off(sequence);
    sequence.update(10301, true, false, true, true, 999);
    off(sequence); // Press during OPEN is not queued for the end.
    press(sequence, 10303, 250);
  }
  {
    BenchSequence sequence;
    const uint32_t start = UINT32_MAX - 5000;
    press(sequence, start, 200);
    sequence.update(start + 9999, true, false, false, true, 200);
    assert(sequence.remainingCountdownMs(start + 9999) == 1);
    sequence.update(start + 10000, true, false, false, true, 200);
    assert(sequence.valveOn());
    assert(sequence.remainingOpenMs(start + 10199) == 1);
    sequence.update(start + 10200, true, false, false, true, 200);
    off(sequence);
  }
  {
    BenchSequence sequence;
    // A pulse that itself crosses millis() rollover also expires correctly.
    const uint32_t start = UINT32_MAX - 10100;
    press(sequence, start, 200);
    sequence.update(start + 10000, true, false, false, true, 200);
    assert(sequence.valveOn());
    sequence.update(start + 10199, true, false, false, true, 200);
    assert(sequence.remainingOpenMs(start + 10199) == 1);
    sequence.update(start + 10200, true, false, false, true, 200);
    off(sequence);
  }
  std::cout << "Bench sequence countdown, Cancel priority, interlocks, rearm and rollover tests passed\n";
}
