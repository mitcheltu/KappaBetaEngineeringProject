#pragma once

#include <cmath>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

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
  uint64_t completedAtUs = 0;
  uint64_t cnnDurationUs = 0;
  uint64_t roundDurationUs = 0;
  ImuRisk::EvaluationResult cnn{};
  SinkRateComparison depth{};
  PpgRiskResult ppg{};
  float combinedRisk = NAN;
  bool gateOpen = false;
  bool fusionComplete = false;
  bool fusionValid = false;
};

// Processing publishes whole rounds; telemetry and diagnostics take short
// copies so neither reader can observe a partially updated result.
class RiskRoundStore {
 public:
  bool begin() {
    if (mutex_) return true;
    mutex_ = xSemaphoreCreateMutex();
    return mutex_ != nullptr;
  }

  bool publish(const RiskRoundResult& result) {
    if (!lock()) return false;
    latest_ = result;
    available_ = true;
    xSemaphoreGive(mutex_);
    return true;
  }

  bool snapshot(RiskRoundResult& result) const {
    if (!lock()) return false;
    const bool available = available_;
    if (available) result = latest_;
    xSemaphoreGive(mutex_);
    return available;
  }

 private:
  bool lock() const {
    return mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE;
  }

  mutable SemaphoreHandle_t mutex_ = nullptr;
  RiskRoundResult latest_{};
  bool available_ = false;
};
