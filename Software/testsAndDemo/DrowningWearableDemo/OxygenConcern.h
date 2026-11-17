#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace Ppg {

// Notebook 03's absolute concern_level variant, using deep_breath_hold anchors.
// This research indicator is not a validated drowning/blackout detector.
struct OxygenConfig {
  float start = 80.0f;
  float full = 60.0f;
  float tauSeconds = 3.0f;
  float enterScore = 0.50f;
  float exitScore = 0.25f;
  uint32_t confirmSeconds = 3;
  uint32_t recoverySeconds = 5;
  bool autoClear = false;  // Device behavior latches until acknowledge().
};

class OxygenConcern {
 public:
  explicit OxygenConcern(const OxygenConfig& config = OxygenConfig()) : config_(config) {
    reset();
  }

  // One estimate per second. True means this sample produced a concern value;
  // false means invalid input or fewer than three contiguous valid estimates.
  // Integers count seconds, not milliseconds. A uint32_t rollover is contiguous.
  bool update(float spo2, bool valid, uint32_t second) {
    const bool accepted = valid && std::isfinite(spo2) && spo2 > 0.0f && spo2 <= 100.0f;
    const bool contiguous = previousValid_ && accepted &&
                            static_cast<uint32_t>(second - previousSecond_) == 1;
    if (!contiguous) restartWindow();
    previousSecond_ = second;
    previousValid_ = accepted;
    concern_ = unavailable();
    if (!accepted || !validConfiguration()) return false;

    samples_[next_] = spo2;
    next_ = static_cast<uint8_t>((next_ + 1) % 3);
    if (count_ < 3) ++count_;
    if (count_ < 3) return false;

    float a = samples_[0], b = samples_[1], c = samples_[2];
    if (a > b) swap(a, b);
    if (b > c) swap(b, c);
    if (a > b) swap(a, b);
    const float severity = clamp((config_.start - b) / (config_.start - config_.full));
    if (!hasState_) {
      state_ = severity;
      hasState_ = true;
    } else {
      const float alpha = 1.0f - std::exp(-1.0f / config_.tauSeconds);
      state_ += alpha * (severity - state_);
    }
    concern_ = state_;

    if (!active_) {
      lowPending_ = false;
      if (state_ >= config_.enterScore) {
        if (!highPending_) { highSince_ = second; highPending_ = true; }
        if (static_cast<uint32_t>(second - highSince_) >= config_.confirmSeconds) {
          active_ = true;
          highPending_ = false;
        }
      } else {
        highPending_ = false;
      }
    } else {
      highPending_ = false;
      if (state_ <= config_.exitScore) {
        if (!lowPending_) { lowSince_ = second; lowPending_ = true; }
        if (config_.autoClear &&
            static_cast<uint32_t>(second - lowSince_) >= config_.recoverySeconds) {
          active_ = false;
          lowPending_ = false;
        }
      } else {
        lowPending_ = false;
      }
    }
    return true;
  }

  // Acknowledgement clears the latch and confirmation/recovery timers. It
  // preserves the estimate: continuing high concern must confirm again.
  void acknowledge() {
    active_ = false;
    highPending_ = lowPending_ = false;
  }

  // A deliberate new session discards both the latch and all filter history.
  void reset() {
    restartWindow();
    previousValid_ = hasState_ = active_ = false;
    previousSecond_ = 0;
    state_ = 0.0f;
    concern_ = unavailable();
  }

  float concern() const { return concern_; }  // NaN while invalid/warming up.
  bool active() const { return active_; }
  bool ready() const { return std::isfinite(concern_); }

 private:
  static float unavailable() { return std::numeric_limits<float>::quiet_NaN(); }
  static void swap(float& a, float& b) { const float t = a; a = b; b = t; }
  static float clamp(float value) {
    return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
  }
  bool validConfiguration() const {
    return std::isfinite(config_.start) && std::isfinite(config_.full) &&
           config_.start > config_.full && std::isfinite(config_.tauSeconds) &&
           config_.tauSeconds > 0.0f && std::isfinite(config_.enterScore) &&
           std::isfinite(config_.exitScore) && config_.exitScore >= 0.0f &&
           config_.exitScore < config_.enterScore && config_.enterScore <= 1.0f;
  }
  void restartWindow() {
    count_ = next_ = 0;
    highPending_ = lowPending_ = false;
    // Notebook replay preserves its exponential state and active latch at gaps.
    // Restarting only the median window/timers reproduces that behavior.
  }

  OxygenConfig config_;
  float samples_[3]{};
  float state_ = 0.0f, concern_ = unavailable();
  uint32_t previousSecond_ = 0, highSince_ = 0, lowSince_ = 0;
  uint8_t next_ = 0, count_ = 0;
  bool previousValid_ = false, hasState_ = false, active_ = false;
  bool highPending_ = false, lowPending_ = false;
};

}  // namespace Ppg
