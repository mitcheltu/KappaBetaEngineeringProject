#ifdef NDEBUG
#error "These assertion tests must never be compiled with -DNDEBUG"
#endif
// DoubleTap.h (detectioncode.md algorithm) driven with synthetic 100 Hz accelerometer
// samples in g: gravity on z plus short jolts. Arithmetic test only.
#include "DoubleTap.h"
#include <cassert>
#include <iostream>
#include <vector>

// Returns the times (ms) at which a double tap was reported.
static std::vector<uint32_t> run(const std::vector<uint32_t>& tapsMs, float jolt = 1.2f,
                                 uint32_t durationMs = 3000, float swayG = 0.0f, uint32_t tapLenMs = 20) {
  DoubleTap d;
  std::vector<uint32_t> hits;
  for (uint32_t t = 0; t < durationMs; t += 10) {
    float z = 1.0f + swayG * ((t / 500) % 2 ? 1.0f : -1.0f);  // slow 1 Hz square sway
    for (uint32_t tap : tapsMs)
      if (t >= tap && t < tap + tapLenMs) z += jolt;
    if (d.update(0.0f, 0.0f, z, t)) hits.push_back(t);
  }
  return hits;
}

int main() {
  // Two taps 250 ms apart -> one double tap, at the second tap.
  auto h = run({1000, 1250});
  assert(h.size() == 1 && h[0] == 1250);
  // Too close (< DT_MIN after the first): the second jolt lands in the window but too early.
  assert(run({1000, 1100}).empty());
  // Too far apart (> DT_MAX): two single taps, no double tap.
  assert(run({1000, 1600}).empty());
  // A single tap never fires.
  assert(run({1000}).empty());
  // Small jolts below TAP_G are ignored.
  assert(run({1000, 1250}, 0.4f).empty());
  // Ringing inside TAP_SETTLE after a tap is not a second tap: one long 60 ms jolt.
  assert(run({1000}, 1.2f, 3000, 0.0f, 60).empty());
  // Slow sway (gravity tracking) of 0.3 g does not count as taps.
  assert(run({}, 1.2f, 6000, 0.3f).empty());
  // Two separate double taps are both reported.
  h = run({1000, 1200, 2500, 2700}, 1.2f, 4000);
  assert(h.size() == 2 && h[0] == 1200 && h[1] == 2700);
  // Non-finite samples are ignored.
  DoubleTap d;
  assert(!d.update(NAN, 0, 1, 0));
  std::cout << "Double tap (detectioncode.md) timing, threshold, settle and sway tests passed\n";
}
