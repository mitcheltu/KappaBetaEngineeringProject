#pragma once

#include <cmath>
#include <cstdint>

#include "ImuRiskEvaluator.h"
#include "PpgSensor.h"
#include "SinkRateEstimator.h"

enum class GateTransition : uint8_t {
  NONE,
  OPENED,
  CLOSED,
};

class CnnRiskGate {
 public:
  explicit CnnRiskGate(float threshold) : threshold_(threshold) {}

  GateTransition observe(const ImuRisk::EvaluationResult& result) {
    if (!result.valid || !std::isfinite(result.score) ||
        result.score < 0.0f || result.score > 1.0f)
      return GateTransition::NONE;

    if (result.score >= threshold_) {
      consecutiveNegatives_ = 0;
      if (!open_) {
        open_ = true;
        return GateTransition::OPENED;
      }
      return GateTransition::NONE;
    }

    if (!open_) return GateTransition::NONE;
    if (++consecutiveNegatives_ < 2) return GateTransition::NONE;
    consecutiveNegatives_ = 0;
    open_ = false;
    return GateTransition::CLOSED;
  }

  bool isOpen() const { return open_; }
  uint8_t consecutiveNegatives() const { return consecutiveNegatives_; }

 private:
  float threshold_;
  uint8_t consecutiveNegatives_ = 0;
  bool open_ = false;
};

struct RiskRoundResult {
  uint32_t roundId = 0;
  uint64_t targetUs = 0;
  ImuRisk::EvaluationResult cnn{};
  SinkRateComparison depth{};
  PpgRiskResult ppg{};
};
