#pragma once
#include <cmath>
#include <stdint.h>
#include "ImuCnn.h"
#include "OxygenTrack.h"
#include "PpgWindow.h"
#include "PulseLoss.h"
#include "ScenarioSource.h"

// Single-owner inference over a scripted Scenario::Source, paced by elapsed
// playback time supplied by the caller (virtual time in host tests, millis()
// on the device). No allocation; the object holds ~110 KB of fixed workspace,
// so keep it in static storage, never on a task stack.
//
// Three tracks, each the project's existing model/rule:
//   IMU     teammate's IMU CNN on 3-s windows every 1 s, through the same gate as
//           microcontrollerFinal's CnnRiskGate: opens at score >= 0.998, closes
//           after 2 consecutive lower scores.
//   pulse   notebook 04 v5 cnn_v5_h on 16-s windows every 2 s (latched alert).
//   oxygen  notebook 03 concern_level, 80 -> 60 % (latched alert).
// Fusion (default, team decision 2026-11-16): fire once when the IMU gate is
// open AND the pulse or oxygen track has alerted. Depth is not a scenario input.
//
// Pulse qualification: ppgSoftware gates pulse inference behind an operator
// qualification (QualityGate). Here the *scripted scenario itself* stands in for
// that qualification: pulse evaluation is permitted whenever a scenario is
// playing, because the input is known scripted data, not a live wearer.
namespace Scenario {
enum class Fusion : uint8_t { IMU_GATE_AND_VITALS = 0, VITALS_ONLY = 1 };

struct InferenceConfig {
  Ppg::PulseModel model = Ppg::PulseModel::CnnV5H;
  Ppg::PulseOperatingPoint point = Ppg::PulseOperatingPoint::HighSpecificity;
  Fusion fusion = Fusion::IMU_GATE_AND_VITALS;
  uint32_t stepSamples = 200;       // Re-evaluate the pulse every 2 s of new IR.
  uint32_t imuStrideSamples = 20;   // Re-evaluate the IMU every 1 s (20 rows).
  uint8_t gateCloseAfter = 2;       // Consecutive below-threshold IMU scores.
  uint32_t budgetUs = 2000000;      // Counted as an overrun when exceeded.
};

struct PulseOutcome {
  bool valid = false, alert = false, overBudget = false;
  float score = NAN;
  uint32_t analysisUs = 0, elapsedMs = 0, windowEndSample = 0;
};
struct OxygenOutcome {
  bool produced = false, valid = false, active = false;
  float spo2 = NAN, concern = NAN;
  uint32_t second = 0, elapsedMs = 0;
};
struct ImuOutcome {
  bool valid = false, gateOpen = false;
  float score = NAN;
  uint32_t analysisUs = 0, elapsedMs = 0;
};
struct Step {
  bool pulse = false, oxygen = false, imu = false;
  PulseOutcome pulseOutcome;
  OxygenOutcome oxygenOutcome;
  ImuOutcome imuOutcome;
};

class Inference {
 public:
  using ClockUs = uint64_t (*)();
  static constexpr int IMU_WINDOW = Imu::Cnn::Samples, IMU_AXES = Imu::Cnn::Axes;

  Inference(OxygenTrack& oxygen, ClockUs clock, const InferenceConfig& config = InferenceConfig())
      : oxygen_(oxygen), clock_(clock), config_(config) {}

  // Start a playback on an already-selected source.
  void begin(Source* source) {
    source_ = source;
    ring_.reset();
    oxygen_.reset();
    nextIr_ = nextSpo2_ = nextImu_ = sinceEval_ = 0;
    imuFill_ = imuHead_ = 0;
    pulseActive_ = gateOpen_ = fused_ = false;
    gateMisses_ = 0;
    firstFill_ = true;
    lastScore_ = lastImuScore_ = NAN;
  }
  void stop() { source_ = nullptr; }

  bool finished() const {
    return !source_ || (nextIr_ >= source_->irCount() && nextSpo2_ >= source_->spo2Count() &&
                        nextImu_ >= source_->imuCount());
  }

  // Consume samples due by elapsedMs, in time order, stopping after the first
  // produced result so every result can be reported. Call repeatedly until it
  // returns a Step with no flag set. Ties: SpO2, then IMU, then IR.
  Step advance(uint32_t elapsedMs) {
    Step step;
    if (!source_) return step;
    for (;;) {
      const uint64_t tSpo2 = uint64_t(nextSpo2_) * 1000u, tImu = uint64_t(nextImu_) * 50u,
                     tIr = uint64_t(nextIr_) * 10u;
      const bool spo2Due = nextSpo2_ < source_->spo2Count() && tSpo2 <= elapsedMs;
      const bool imuDue = nextImu_ < source_->imuCount() && tImu <= elapsedMs;
      const bool irDue = nextIr_ < source_->irCount() && tIr <= elapsedMs;
      if (!spo2Due && !imuDue && !irDue) return step;
      if (spo2Due && (!imuDue || tSpo2 <= tImu) && (!irDue || tSpo2 <= tIr)) {
        consumeSpo2(step.oxygenOutcome);
        step.oxygen = true;
        updateFusion();
        return step;
      }
      if (imuDue && (!irDue || tImu <= tIr)) {
        if (consumeImu(step.imuOutcome)) {
          step.imu = true;
          updateFusion();
          return step;
        }
        continue;
      }
      float value = NAN;
      const bool have = source_->ir(nextIr_, value);
      // Synthetic sample clock: exactly 10 ms per sample, never bridged.
      ring_.add(value, uint64_t(nextIr_ + 1) * 10000u, have && std::isfinite(value));
      ++nextIr_;
      // Evaluate when the 16-s ring first fills (or refills after a reset),
      // then after every stepSamples new samples.
      if (!ring_.full()) { firstFill_ = true; sinceEval_ = 0; continue; }
      if (firstFill_) firstFill_ = false;
      else if (++sinceEval_ < config_.stepSamples) continue;
      sinceEval_ = 0;
      evaluatePulse(step.pulseOutcome);
      step.pulse = true;
      updateFusion();
      return step;
    }
  }

  bool pulseActive() const { return pulseActive_; }
  bool oxygenActive() const { return oxygen_.active(); }
  bool imuGateOpen() const { return gateOpen_; }
  // The fused decision that may request the vibration -> pump sequence (latched).
  bool alert() const { return fused_; }
  bool vitalsAlert() const { return pulseActive_ || oxygen_.active(); }
  float lastPulseScore() const { return lastScore_; }
  float lastImuScore() const { return lastImuScore_; }
  float concern() const { return oxygen_.concern(); }
  uint32_t lastAnalysisUs() const { return lastUs_; }
  uint32_t maxAnalysisUs() const { return maxUs_; }
  uint32_t overruns() const { return overruns_; }
  uint32_t evaluations() const { return evaluations_; }
  const InferenceConfig& config() const { return config_; }

 private:
  void consumeSpo2(OxygenOutcome& o) {
    float value = NAN;
    bool valid = false;
    if (!source_->spo2(nextSpo2_, value, valid)) valid = false;
    o.second = nextSpo2_;
    o.elapsedMs = nextSpo2_ * 1000u;
    o.spo2 = valid ? value : NAN;
    o.produced = oxygen_.update(value, valid, nextSpo2_);
    o.concern = oxygen_.concern();
    o.valid = std::isfinite(o.concern);
    o.active = oxygen_.active();
    ++nextSpo2_;
  }

  // Returns true when this row completed a window that was scored.
  bool consumeImu(ImuOutcome& o) {
    float row[IMU_AXES];
    bool ok = source_->imu(nextImu_, row);
    for (int a = 0; ok && a < IMU_AXES; ++a) ok = std::isfinite(row[a]);
    const uint32_t index = nextImu_++;
    if (!ok) { imuFill_ = 0; return false; }  // A gap restarts the window, never bridged.
    for (int a = 0; a < IMU_AXES; ++a) imuRing_[imuHead_][a] = row[a];
    imuHead_ = (imuHead_ + 1) % IMU_WINDOW;
    if (imuFill_ < UINT32_MAX) ++imuFill_;
    if (imuFill_ < uint32_t(IMU_WINDOW) ||
        (imuFill_ - IMU_WINDOW) % config_.imuStrideSamples != 0) return false;
    for (int t = 0; t < IMU_WINDOW; ++t)
      for (int a = 0; a < IMU_AXES; ++a)
        imuWindow_[t * IMU_AXES + a] = imuRing_[(imuHead_ + t) % IMU_WINDOW][a];
    const uint64_t begin = clock_ ? clock_() : 0;
    const float score = imuCnn_.score(imuWindow_);
    const uint64_t end = clock_ ? clock_() : 0;
    o.analysisUs = end >= begin && end - begin <= UINT32_MAX ? uint32_t(end - begin) : UINT32_MAX;
    o.elapsedMs = index * 50u;
    o.valid = std::isfinite(score);
    o.score = score;
    if (o.valid) {
      if (Imu::Cnn::gate(score)) {
        gateOpen_ = true;
        gateMisses_ = 0;
      } else if (gateOpen_ && ++gateMisses_ >= config_.gateCloseAfter) {
        gateOpen_ = false;
        gateMisses_ = 0;
      }
    }
    lastImuScore_ = score;
    o.gateOpen = gateOpen_;
    return true;
  }

  void updateFusion() {
    if (fused_ || !vitalsAlert()) return;
    fused_ = config_.fusion == Fusion::VITALS_ONLY || gateOpen_;
  }

  void evaluatePulse(PulseOutcome& out) {
    out.windowEndSample = nextIr_;
    out.elapsedMs = (nextIr_ - 1) * 10u;
    if (!ring_.copy(window_)) return;
    const uint64_t begin = clock_ ? clock_() : 0;
    const Ppg::PulseResult result = detector_.evaluate(window_, config_.model, config_.point);
    const uint64_t end = clock_ ? clock_() : 0;
    const uint64_t spent = end >= begin ? end - begin : 0;
    out.analysisUs = spent > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(spent);
    lastUs_ = out.analysisUs;
    if (out.analysisUs > maxUs_) maxUs_ = out.analysisUs;
    out.overBudget = out.analysisUs > config_.budgetUs;
    if (out.overBudget) ++overruns_;
    ++evaluations_;
    out.valid = result.valid;
    out.score = result.valid ? static_cast<float>(result.score) : NAN;
    out.alert = result.valid && result.alert;
    lastScore_ = out.score;
    if (out.alert) pulseActive_ = true;  // Latched for the rest of the run.
  }

  OxygenTrack& oxygen_;
  ClockUs clock_;
  InferenceConfig config_;
  Source* source_ = nullptr;
  Ppg::PpgWindow ring_;
  Ppg::PulseLoss detector_;
  Imu::Cnn imuCnn_;
  float window_[Ppg::PpgWindow::kSamples] = {};
  float imuRing_[IMU_WINDOW][IMU_AXES] = {};
  float imuWindow_[IMU_WINDOW * IMU_AXES] = {};
  uint32_t nextIr_ = 0, nextSpo2_ = 0, nextImu_ = 0, sinceEval_ = 0;
  uint32_t imuFill_ = 0, imuHead_ = 0;
  uint32_t lastUs_ = 0, maxUs_ = 0, overruns_ = 0, evaluations_ = 0;
  float lastScore_ = NAN, lastImuScore_ = NAN;
  uint8_t gateMisses_ = 0;
  bool pulseActive_ = false, gateOpen_ = false, fused_ = false, firstFill_ = true;
};
}  // namespace Scenario
