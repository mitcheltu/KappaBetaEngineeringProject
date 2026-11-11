#pragma once

#include <cmath>
#include <cstdint>

#include "FirmwareConfig.h"

struct PpgReading {
  uint32_t red = 0;
  uint32_t infrared = 0;
  uint64_t timestampUs = 0;
  bool valid = false;
};

struct PpgAnalysis {
  int32_t heartRateBpm = 0;
  int32_t oxygenSaturationPercent = 0;
  uint64_t timestampUs = 0;
  uint64_t inputStartUs = 0;
  uint64_t inputEndUs = 0;
  bool heartRateValid = false;
  bool oxygenSaturationValid = false;
};

struct PpgRiskResult {
  float score = NAN;
  uint64_t timestampUs = 0;
  uint64_t inputStartUs = 0;
  uint64_t inputEndUs = 0;
  bool valid = false;
  bool usedDummy = false;
};

inline bool ppgSignalAccepted(uint32_t red, uint32_t infrared) {
  return infrared >= Config::PPG_MIN_IR && red != 0 &&
         infrared < Config::PPG_MAX_ADC && red < Config::PPG_MAX_ADC;
}

// Application-facing MAX30102 interface. The MAX3010x driver remains
// responsible for bus access; this object publishes timestamped readings and
// exposes the explicitly configured risk placeholder until a real mapper is
// available.
class PpgSensor {
 public:
  explicit PpgSensor(float dummyRiskScore = Config::PPG_DUMMY_RISK_SCORE)
      : dummyRiskScore_(dummyRiskScore) {}

  void publishReading(uint32_t red, uint32_t infrared, uint64_t timestampUs) {
    reading_.red = red;
    reading_.infrared = infrared;
    reading_.timestampUs = timestampUs;
    reading_.valid = true;
  }

  void invalidateReading(uint64_t timestampUs) {
    reading_.red = 0;
    reading_.infrared = 0;
    reading_.timestampUs = timestampUs;
    reading_.valid = false;
  }

  void publishAnalysis(int32_t heartRateBpm, int32_t oxygenSaturationPercent,
                       bool heartRateValid, bool oxygenSaturationValid,
                       uint64_t timestampUs) {
    publishAnalysis(heartRateBpm, oxygenSaturationPercent, heartRateValid,
                    oxygenSaturationValid, timestampUs, timestampUs);
  }

  void publishAnalysis(int32_t heartRateBpm, int32_t oxygenSaturationPercent,
                       bool heartRateValid, bool oxygenSaturationValid,
                       uint64_t inputStartUs, uint64_t inputEndUs) {
    analysis_.heartRateBpm = heartRateBpm;
    analysis_.oxygenSaturationPercent = oxygenSaturationPercent;
    analysis_.timestampUs = inputEndUs;
    analysis_.inputStartUs = inputStartUs;
    analysis_.inputEndUs = inputEndUs;
    analysis_.heartRateValid = heartRateValid;
    analysis_.oxygenSaturationValid = oxygenSaturationValid;
  }

  void invalidateAnalysis(uint64_t timestampUs) {
    analysis_.heartRateBpm = 0;
    analysis_.oxygenSaturationPercent = 0;
    analysis_.timestampUs = timestampUs;
    analysis_.inputStartUs = 0;
    analysis_.inputEndUs = 0;
    analysis_.heartRateValid = false;
    analysis_.oxygenSaturationValid = false;
  }

  const PpgReading& reading() const { return reading_; }
  const PpgAnalysis& analysis() const { return analysis_; }
  bool hasReading() const { return reading_.valid; }

  bool setDummyRiskScore(float score) {
    if (!std::isfinite(score) || score < 0.0f || score > 1.0f) return false;
    dummyRiskScore_ = score;
    return true;
  }

  float dummyRiskScore() const { return dummyRiskScore_; }

  PpgRiskResult riskResult(uint64_t timestampUs = 0,
                           uint64_t inputStartUs = 0,
                           uint64_t inputEndUs = 0,
                           bool inputValid = true) const {
    PpgRiskResult result;
    result.score = dummyRiskScore_;
    result.timestampUs = timestampUs != 0 ? timestampUs : reading_.timestampUs;
    result.inputStartUs = inputStartUs;
    result.inputEndUs = inputEndUs;
    result.valid = inputValid && std::isfinite(dummyRiskScore_) &&
                   dummyRiskScore_ >= 0.0f && dummyRiskScore_ <= 1.0f;
    result.usedDummy = true;
    return result;
  }

 private:
  PpgReading reading_{};
  PpgAnalysis analysis_{};
  float dummyRiskScore_ = Config::PPG_DUMMY_RISK_SCORE;
};
