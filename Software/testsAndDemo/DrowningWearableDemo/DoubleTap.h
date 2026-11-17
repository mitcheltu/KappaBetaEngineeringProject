#pragma once
#include <cmath>
#include <stdint.h>

// Double-tap detector from detectioncode.md (team algorithm, same constants), made
// host-testable: the caller passes each accelerometer sample's time instead of the
// detector calling millis(). The demo feeds every 100 Hz BMI270 FIFO sample, so taps are
// still seen while the depth sensor's ~40 ms blocking read stalls the Arduino loop.
//
//   jolt = | |a| - gBase |, with gBase a slow average of |a| (gravity), in g
//   a tap is a jolt >= TAP_G that is at least TAP_SETTLE after the previous tap;
//   a double tap is a second tap DT_MIN..DT_MAX after the first.
class DoubleTap {
 public:
  static constexpr float TAP_G = 0.6f;          // jolt size that counts as a tap (raise if swimming triggers it)
  static constexpr uint32_t TAP_SETTLE = 80;    // ignore ringing for this long after a tap (ms)
  static constexpr uint32_t DT_MIN = 120;       // second tap at least this long after the first...
  static constexpr uint32_t DT_MAX = 500;       // ...and no later than this (ms)

  // x, y, z in g. Returns true exactly when this sample completes a double tap.
  bool update(float x, float y, float z, uint32_t nowMs) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    const float m = std::sqrt(x * x + y * y + z * z);
    const float jolt = std::fabs(m - gBase_);
    gBase_ += 0.02f * (m - gBase_);  // slow follow, so motion doesn't count as taps

    if (waitingSecond_ && nowMs - firstTap_ > DT_MAX) waitingSecond_ = false;  // too slow, start over
    if (jolt < TAP_G || (haveTap_ && nowMs - lastTap_ < TAP_SETTLE)) return false;  // not a tap, or ringing
    lastTap_ = nowMs;
    haveTap_ = true;
    if (!waitingSecond_) { firstTap_ = nowMs; waitingSecond_ = true; return false; }  // first tap
    if (nowMs - firstTap_ >= DT_MIN) { waitingSecond_ = false; ++count_; return true; }  // second tap in window
    return false;
  }

  void reset() { gBase_ = 1.0f; waitingSecond_ = haveTap_ = false; lastTap_ = firstTap_ = 0; }
  uint32_t count() const { return count_; }

 private:
  float gBase_ = 1.0f;
  uint32_t lastTap_ = 0, firstTap_ = 0, count_ = 0;
  bool waitingSecond_ = false, haveTap_ = false;
};
