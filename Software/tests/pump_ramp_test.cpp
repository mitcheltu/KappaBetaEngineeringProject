#include "PumpRamp.h"
#include "DemoSequence.h"
#include <cassert>
#include <iostream>

static void start(DemoSequence& sequence, uint32_t now) {
  sequence.update(now - 1, true, false, false, true);
  sequence.update(now, true, false, true, true);
  sequence.update(now + 10000, true, false, false, true);
  assert(sequence.pumpOn());
}

int main() {
  static_assert(Config::PPG_INT == 4 && Config::IMU_INT1 == 15 && Config::PUMP_IN1 == 13,
                "Rev J pin map changed");
  static_assert(Config::PUMP_PWM_HZ == 20000 && Config::PUMP_RAMP_MS == 2000);
  PumpRamp ramp;
  assert(ramp.update(0, false) == 0);
  assert(ramp.update(500, true) == 0);
  uint32_t previous = 0;
  for (uint32_t elapsed = 0; elapsed <= 2000; ++elapsed) {
    const auto duty = ramp.update(500 + elapsed, true);
    assert(duty == Config::PUMP_MAX_DUTY * elapsed / 2000);
    assert(duty >= previous && duty <= Config::PUMP_MAX_DUTY);
    previous = duty;
  }
  assert(ramp.update(60000, true) == Config::PUMP_MAX_DUTY);
  assert(ramp.update(60001, false) == 0);
  assert(ramp.update(60002, true) == 0);
  assert(ramp.update(61002, true) == 511);
  assert(ramp.update(61003, false) == 0);
  // A delayed update advances to elapsed duty without extending the ramp.
  assert(ramp.update(70000, true) == 0);
  assert(ramp.update(73000, true) == Config::PUMP_MAX_DUTY);
  ramp.update(0, false);
  constexpr uint32_t nearWrap = UINT32_MAX - 1000;
  assert(ramp.update(nearWrap, true) == 0);
  assert(ramp.update(nearWrap + 1000u, true) == 511);
  assert(ramp.update(nearWrap + 2000u, true) == Config::PUMP_MAX_DUTY);

  // Cancel, low battery, disabled control and normal timeout stop the ramp.
  for (uint32_t stopAt : {0u, 1000u, 2000u, 60000u}) {
    for (unsigned cause = 0; cause < 3; ++cause) {
      DemoSequence sequence;
      PumpRamp output;
      start(sequence, 100);
      assert(output.update(10100, sequence.pumpOn()) == 0);
      sequence.update(10100 + stopAt, cause != 2, cause == 0, false, cause != 1);
      assert(!sequence.pumpOn());
      assert(output.update(10100 + stopAt, sequence.pumpOn()) == 0);
      sequence.update(10101 + stopAt, true, false, true, true);
      assert(!sequence.pumpOn()); // Stop requires release/repress, never resumes.
    }
  }
  DemoSequence sequence;
  PumpRamp output;
  start(sequence, 100);
  output.update(10100, sequence.pumpOn());
  sequence.update(12100, true, false, false, true);
  assert(output.update(12100, sequence.pumpOn()) == Config::PUMP_MAX_DUTY);
  assert(sequence.remainingPhaseMs(12100) == 58000);
  sequence.update(70100, true, false, false, true);
  assert(output.update(70100, sequence.pumpOn()) == 0);
  std::cout << "Rev J pins, 20 kHz/2 s ramp, restart, rollover, interlocks and 60 s timeout passed\n";
}
