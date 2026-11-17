#include "DemoSequence.h"
#include <cassert>
#include <iostream>

using Stage = DemoSequence::Stage;

void assertIdle(const DemoSequence& sequence, uint32_t now) {
  assert(sequence.stage() == Stage::IDLE);
  assert(!sequence.motorOn(now));
  assert(!sequence.pumpOn());
  assert(sequence.remainingPhaseMs(now) == 0);
}

void freshPress(DemoSequence& sequence, uint32_t now) {
  sequence.update(now - 1, true, false, false, true);
  assert(sequence.canAcceptSos());
  sequence.update(now, true, false, true, true);
  assert(sequence.stage() == Stage::VIBRATING);
  assert(sequence.motorOn(now));
  assert(!sequence.pumpOn());
  assert(!sequence.canAcceptSos());
  assert(sequence.remainingPhaseMs(now) == 10000);
}

int main() {
  {
    DemoSequence sequence;
    assertIdle(sequence, 0);
    assert(!sequence.canAcceptSos());
    sequence.update(0, true, false, true, true);
    sequence.update(100000, true, false, true, true);
    assertIdle(sequence, 100000); // Held at boot cannot start.
    freshPress(sequence, 100002);
  }

  {
    DemoSequence sequence;
    constexpr uint32_t start = 100;
    freshPress(sequence, start);
    // Check every millisecond, including 999/1000/1499/1500/9999: seven
    // 1000 ms pulses, separated by six 500 ms off periods.
    uint32_t totalOnMs = 0;
    for (uint32_t elapsed = 0; elapsed < 10000; ++elapsed) {
      sequence.update(start + elapsed, true, false, false, true);
      assert(sequence.stage() == Stage::VIBRATING);
      const bool expectedOn = elapsed % 1500 < 1000;
      assert(sequence.motorOn(start + elapsed) == expectedOn);
      assert(!sequence.pumpOn());
      assert(!sequence.canAcceptSos());
      assert(sequence.remainingPhaseMs(start + elapsed) == 10000 - elapsed);
      if (expectedOn) ++totalOnMs;
    }
    assert(totalOnMs == 7000);
    // An overdue output query cannot leave the vibration motor on.
    assert(!sequence.motorOn(start + 10000));
    sequence.update(start + 10000, true, false, true, true);
    assert(sequence.stage() == Stage::PUMPING);
    assert(sequence.pumpOn());
    assert(!sequence.motorOn(start + 10000));
    assert(sequence.remainingPhaseMs(start + 10000) == 60000);
    sequence.update(start + 69999, true, false, true, true);
    assert(sequence.pumpOn());
    assert(sequence.remainingPhaseMs(start + 69999) == 1);
    sequence.update(start + 70000, true, false, true, true);
    assertIdle(sequence, start + 70000);
    sequence.update(start + 100000, true, false, true, true);
    assertIdle(sequence, start + 100000); // Held SOS cannot repeat the demo.
    freshPress(sequence, start + 100002);
  }

  // Cancel during on/off pulses, at either transition, and during pumping.
  for (uint32_t elapsed : {0u, 999u, 1000u, 1499u, 1500u, 9999u,
                           10000u, 10001u, 69999u, 70000u}) {
    DemoSequence sequence;
    constexpr uint32_t start = 100;
    freshPress(sequence, start);
    if (elapsed > 10000) sequence.update(start + 10000, true, false, true, true);
    sequence.update(start + elapsed, true, true, true, true);
    assertIdle(sequence, start + elapsed);
    assert(!sequence.canAcceptSos());
    sequence.update(start + elapsed + 1, true, false, true, true);
    assertIdle(sequence, start + elapsed + 1);
    freshPress(sequence, start + elapsed + 3);
  }

  {
    DemoSequence sequence;
    sequence.update(0, true, false, false, true);
    sequence.update(1, true, true, true, true); // Simultaneous SOS and Cancel.
    assertIdle(sequence, 1);
    sequence.update(2, true, false, true, true);
    assertIdle(sequence, 2);
    freshPress(sequence, 4);
  }

  // Low battery and disabled outputs block startup and abort either phase.
  for (bool disabled : {false, true}) {
    DemoSequence sequence;
    sequence.update(0, true, false, false, true);
    sequence.update(1, !disabled, false, true, disabled);
    assertIdle(sequence, 1);
    sequence.update(2, true, false, true, true);
    assertIdle(sequence, 2);
    freshPress(sequence, 4);
    for (bool pumping : {false, true}) {
      DemoSequence active;
      freshPress(active, 100);
      if (pumping) active.update(10100, true, false, true, true);
      const uint32_t now = pumping ? 10101 : 1100;
      active.update(now, !disabled, false, true, disabled);
      assertIdle(active, now);
      active.update(now + 1, true, false, true, true);
      assertIdle(active, now + 1);
      freshPress(active, now + 3);
    }
  }

  {
    DemoSequence sequence;
    freshPress(sequence, 100);
    sequence.update(4000, true, false, false, true);
    sequence.update(4001, true, false, true, true); // Active re-press ignored.
    assert(sequence.remainingPhaseMs(4001) == 6099);
    sequence.update(10100, true, false, false, true);
    assert(sequence.pumpOn());
    sequence.update(30000, true, false, true, true); // Cannot extend pumping.
    sequence.update(70099, true, false, true, true);
    assert(sequence.remainingPhaseMs(70099) == 1);
    sequence.update(70100, true, false, true, true);
    assertIdle(sequence, 70100);
    sequence.update(70101, true, false, true, true);
    assertIdle(sequence, 70101);
  }

  {
    DemoSequence sequence;
    freshPress(sequence, 0);
    // A late transition starts a complete 60-second pump phase at 10.250 s.
    sequence.update(10250, true, false, false, true);
    assert(sequence.pumpOn());
    assert(sequence.remainingPhaseMs(10250) == 60000);
    sequence.update(70249, true, false, false, true);
    assert(sequence.pumpOn());
    assert(sequence.remainingPhaseMs(70249) == 1);
    sequence.update(70250, true, false, false, true);
    assertIdle(sequence, 70250);
  }

  // Exercise rollover during vibration and, separately, during pumping.
  for (uint32_t start : {UINT32_MAX - 500u, UINT32_MAX - 11000u}) {
    DemoSequence sequence;
    freshPress(sequence, start);
    for (uint32_t elapsed : {999u, 1000u, 1499u, 1500u, 9999u}) {
      sequence.update(start + elapsed, true, false, false, true);
      assert(sequence.stage() == Stage::VIBRATING);
      assert(sequence.motorOn(start + elapsed) == (elapsed % 1500 < 1000));
      assert(sequence.remainingPhaseMs(start + elapsed) == 10000 - elapsed);
    }
    sequence.update(start + 10000u, true, false, false, true);
    assert(sequence.pumpOn());
    sequence.update(start + 69999u, true, false, false, true);
    assert(sequence.remainingPhaseMs(start + 69999u) == 1);
    sequence.update(start + 70000u, true, false, false, true);
    assertIdle(sequence, start + 70000u);
  }

  // ---- INFERENCE trigger (scripted scenario playback only) ----
  using Trigger = DemoSequence::Trigger;
  {
    DemoSequence sequence;
    sequence.update(0, true, false, false, true);
    assert(sequence.trigger() == Trigger::NONE);
    assert(!sequence.requestInference(1, true, false, true, 0, 0));   // No run.
    assert(!sequence.requestInference(1, true, false, true, 5, 6));   // Not the playing run.
    assert(!sequence.requestInference(1, true, false, true, 5, 0));   // Nothing playing.
    assert(!sequence.requestInference(1, true, true, true, 5, 5));    // Cancel.
    assert(!sequence.requestInference(1, false, false, true, 5, 5));  // Outputs disabled.
    assert(!sequence.requestInference(1, true, false, false, 5, 5));  // Battery interlock.
    assertIdle(sequence, 1);
    assert(sequence.requestInference(2, true, false, true, 5, 5));
    assert(sequence.stage() == Stage::VIBRATING && sequence.trigger() == Trigger::INFERENCE);
    assert(sequence.motorOn(2) && !sequence.pumpOn() && sequence.remainingPhaseMs(2) == 10000);
    // Same cadence and phase lengths as the SOS sequence.
    uint32_t onMs = 0;
    for (uint32_t elapsed = 0; elapsed < 10000; ++elapsed) {
      sequence.update(2 + elapsed, true, false, false, true);
      assert(sequence.motorOn(2 + elapsed) == (elapsed % 1500 < 1000));
      if (sequence.motorOn(2 + elapsed)) ++onMs;
    }
    assert(onMs == 7000);
    sequence.update(10002, true, false, false, true);
    assert(sequence.pumpOn() && sequence.trigger() == Trigger::INFERENCE);
    assert(!sequence.requestInference(10003, true, false, true, 7, 7));  // Cannot restart/extend.
    sequence.update(40000, true, false, true, true);  // SOS during inference pump is ignored.
    assert(sequence.remainingPhaseMs(40000) == 30002);
    sequence.update(70001, true, false, false, true);
    assert(sequence.pumpOn());
    sequence.update(70002, true, false, false, true);
    assertIdle(sequence, 70002);
    assert(sequence.trigger() == Trigger::NONE);
    assert(!sequence.requestInference(70003, true, false, true, 5, 5));  // Once per run.
    assert(sequence.requestInference(70004, true, false, true, 6, 6));   // A new run may trigger.
  }

  // Cancel aborts an inference sequence at every phase boundary; the same run
  // can never re-trigger, and SOS still needs a fresh press afterwards.
  for (uint32_t elapsed : {0u, 999u, 1000u, 1499u, 9999u, 10000u, 10001u, 69999u}) {
    DemoSequence sequence;
    constexpr uint32_t start = 100;
    sequence.update(start - 1, true, false, false, true);
    assert(sequence.requestInference(start, true, false, true, 9, 9));
    if (elapsed >= 10000) sequence.update(start + 10000, true, false, false, true);
    sequence.update(start + elapsed, true, true, false, true);
    assertIdle(sequence, start + elapsed);
    assert(!sequence.requestInference(start + elapsed + 1, true, false, true, 9, 9));
    freshPress(sequence, start + elapsed + 3);
  }

  // Battery interlock and disabled outputs stop an inference sequence.
  for (bool disabled : {false, true}) {
    for (bool pumping : {false, true}) {
      DemoSequence sequence;
      sequence.update(0, true, false, false, true);
      assert(sequence.requestInference(1, true, false, true, 3, 3));
      if (pumping) sequence.update(10001, true, false, false, true);
      const uint32_t now = pumping ? 10002 : 500;
      sequence.update(now, !disabled, false, false, disabled);
      assertIdle(sequence, now);
    }
  }

  // An SOS sequence blocks the inference trigger; a held SOS cannot start a
  // sequence after an inference sequence finishes.
  {
    DemoSequence sequence;
    freshPress(sequence, 100);
    assert(sequence.trigger() == Trigger::SOS);
    assert(!sequence.requestInference(200, true, false, true, 4, 4));
    assert(sequence.trigger() == Trigger::SOS);
  }
  {
    DemoSequence sequence;
    sequence.update(0, true, false, true, true);  // SOS held from boot: not armed.
    assert(sequence.requestInference(1, true, false, true, 2, 2));
    sequence.update(10001, true, false, true, true);
    sequence.update(70001, true, false, true, true);
    assertIdle(sequence, 70001);
    sequence.update(80000, true, false, true, true);
    assertIdle(sequence, 80000);  // Still held: no SOS sequence.
    freshPress(sequence, 80002);
  }

  std::cout << "Demo motor cadence, timed pump, Cancel priority, interlocks, "
               "SOS rearm, rollover and INFERENCE trigger tests passed\n";
}
