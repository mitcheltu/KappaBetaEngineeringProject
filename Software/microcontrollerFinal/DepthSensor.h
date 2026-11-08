#pragma once

#include <Arduino.h>
#include <MS5837.h>
#include <Wire.h>
#include <cmath>
#include <cstddef>
#include <cstdint>

#ifndef MS5837_H_BLUEROBOTICS
#error "Select BlueRobotics MS5837 Library by BlueRobotics; another MS5837 library was found."
#endif

// Quality is descriptive sensor state. It is deliberately separate from any
// fusion weights, action thresholds or risk scores.
using DepthQualityFlags = uint16_t;
namespace DepthQuality {
constexpr DepthQualityFlags NONE = 0;
constexpr DepthQualityFlags SENSOR_INVALID = 1u << 0;
constexpr DepthQualityFlags I2C_ERROR = 1u << 1;
constexpr DepthQualityFlags MISSING_BASELINE = 1u << 2;
constexpr DepthQualityFlags OUTLIER_REJECTED = 1u << 3;
constexpr DepthQualityFlags STALE = 1u << 4;
constexpr DepthQualityFlags RATE_UNAVAILABLE = 1u << 5;
constexpr DepthQualityFlags FILTER_WARMUP = 1u << 6;
constexpr DepthQualityFlags STOPPED = 1u << 7;
}

struct DepthSensorConfig {
  uint32_t samplePeriodMs = 200; // PROVISIONAL/TBD; official library is blocking.
  float waterDensityKgM3 = 1000.0f; // PROVISIONAL/TBD freshwater/pool density.

  // These filter values are implementation defaults only, not safety-calibrated.
  uint8_t hampelWindowSamples = 5; // PROVISIONAL/TBD.
  float hampelDeviationThreshold = 3.0f; // PROVISIONAL/TBD MAD multiplier.
  float hampelMinimumDeviationM = 0.01f; // PROVISIONAL/TBD MAD floor.
  float emaAlpha = 0.25f; // PROVISIONAL/TBD.
  uint32_t regressionWindowMs = 2500; // PROVISIONAL/TBD; required range is 2-3 s.
  uint8_t minimumRegressionSamples = 3; // PROVISIONAL/TBD.
  uint32_t staleAfterMs = 1000; // PROVISIONAL/TBD.

  float minimumPressurePa = 0.0f;
  float maximumPressurePa = 3000000.0f;
  float minimumTemperatureC = -20.0f;
  float maximumTemperatureC = 85.0f;
};

struct DepthReading {
  uint64_t timestampUs = 0;
  float pressurePa = NAN;
  float temperatureC = NAN;
  float surfacePressurePa = NAN;
  float rawDepthM = NAN;
  float filteredDepthM = NAN;
  float depthRateMps = NAN;
  bool sensorValid = false;
  bool depthValid = false;
  bool rateValid = false;
  DepthQualityFlags qualityFlags = DepthQuality::SENSOR_INVALID |
                                    DepthQuality::MISSING_BASELINE |
                                    DepthQuality::RATE_UNAVAILABLE;
};

class DepthSensor {
 public:
  explicit DepthSensor(const DepthSensorConfig& config = DepthSensorConfig{});

  // The official MS5837 1.1.1 implementation uses the global Wire object and
  // the sensor's fixed 0x76 address internally. The arguments mirror ImuSensor
  // for call-site consistency and identify the bus/address being configured.
  bool begin(TwoWire& bus, uint8_t address);

  // Establishes a fresh surface-pressure baseline and clears dive-only state.
  bool startDive();
  void stopDive();

  // Scheduling is owned by the object; update() performs one sensor transaction
  // only when the integration layer determines the object is due.
  bool due(uint64_t nowUs) const;
  bool update();

  bool ready() const { return ready_; }
  bool diveActive() const { return diveActive_; }
  bool hasReading() const { return reading_.timestampUs != 0; }
  const DepthReading& reading() const { return reading_; }
  DepthReading snapshot(uint64_t nowUs) const;
  bool isStale(uint64_t nowUs) const;
  const DepthSensorConfig& config() const { return config_; }

 private:
  static constexpr size_t HISTORY_CAPACITY = 64;

  struct HistorySample {
    uint64_t timestampUs = 0;
    float depthM = NAN;
    bool valid = false;
  };

  void invalidateReading(DepthQualityFlags flags);
  void invalidateReading(uint64_t timestampUs, float pressurePa,
                         float temperatureC, DepthQualityFlags flags);
  void resetDiveState();
  bool sensorPresent() const;
  bool readPressure(float& pressurePa, float& temperatureC,
                    DepthQualityFlags& failureFlags);
  void processSample(uint64_t timestampUs, float pressurePa, float temperatureC);
  void updateDepthRate(uint64_t timestampUs);
  bool isHampelOutlier(float depthM, bool& warming);
  void appendRawHistory(float depthM, uint64_t timestampUs);
  void appendFilteredHistory(float depthM, uint64_t timestampUs);

  MS5837 driver_;
  DepthSensorConfig config_;
  TwoWire* bus_ = nullptr;
  uint8_t address_ = 0;
  bool ready_ = false;
  bool diveActive_ = false;
  float surfacePressurePa_ = NAN;
  float previousFilteredDepthM_ = NAN;
  bool filteredDepthValid_ = false;
  uint64_t lastSampleUs_ = 0;
  uint64_t lastValidSampleUs_ = 0;
  DepthReading reading_{};
  HistorySample rawHistory_[HISTORY_CAPACITY]{};
  HistorySample filteredHistory_[HISTORY_CAPACITY]{};
  size_t rawHistoryCount_ = 0;
  size_t filteredHistoryCount_ = 0;
};
