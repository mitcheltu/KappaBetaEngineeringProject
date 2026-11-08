#include "DepthSensor.h"

#include <algorithm>
#include <esp_timer.h>

namespace {
constexpr uint8_t MS5837_ADDRESS = 0x76;
constexpr float GRAVITY_MPS2 = 9.80665f;
constexpr float MAD_TO_STANDARD_DEVIATION = 1.4826f;

uint64_t nowUs() {
  return static_cast<uint64_t>(esp_timer_get_time());
}

float median(float* values, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const float value = values[i];
    size_t position = i;
    while (position > 0 && values[position - 1] > value) {
      values[position] = values[position - 1];
      --position;
    }
    values[position] = value;
  }
  if ((count & 1u) != 0u) return values[count / 2u];
  return (values[count / 2u - 1u] + values[count / 2u]) * 0.5f;
}
} // namespace

DepthSensor::DepthSensor(const DepthSensorConfig& config) : config_(config) {
  resetDiveState();
}

bool DepthSensor::begin(TwoWire& bus, uint8_t address) {
  ready_ = false;
  bus_ = &bus;
  address_ = address;
  resetDiveState();

  // BlueRobotics 1.1.1 uses the global Wire instance and hardcodes 0x76.
  if (bus_ != &Wire || address_ != MS5837_ADDRESS) {
    invalidateReading(DepthQuality::I2C_ERROR | DepthQuality::SENSOR_INVALID);
    return false;
  }

  driver_.setModel(MS5837::MS5837_30BA);
  if (!sensorPresent() || !driver_.init()) {
    invalidateReading(DepthQuality::I2C_ERROR | DepthQuality::SENSOR_INVALID);
    return false;
  }

  driver_.setModel(MS5837::MS5837_30BA);
  driver_.setFluidDensity(config_.waterDensityKgM3);
  ready_ = true;
  invalidateReading(DepthQuality::MISSING_BASELINE | DepthQuality::RATE_UNAVAILABLE);
  return true;
}

bool DepthSensor::startDive() {
  resetDiveState();
  if (!ready_) {
    invalidateReading(DepthQuality::I2C_ERROR | DepthQuality::SENSOR_INVALID |
                      DepthQuality::MISSING_BASELINE);
    return false;
  }

  float pressurePa = NAN;
  float temperatureC = NAN;
  DepthQualityFlags failureFlags = DepthQuality::SENSOR_INVALID;
  const uint64_t timestampUs = nowUs();
  lastSampleUs_ = timestampUs;
  if (!readPressure(pressurePa, temperatureC, failureFlags)) {
    ready_ = false;
    invalidateReading(timestampUs, pressurePa, temperatureC,
                      failureFlags | DepthQuality::MISSING_BASELINE);
    return false;
  }

  surfacePressurePa_ = pressurePa;
  diveActive_ = true;
  lastValidSampleUs_ = timestampUs;
  processSample(timestampUs, pressurePa, temperatureC);
  return reading_.sensorValid && reading_.depthValid;
}

void DepthSensor::stopDive() {
  resetDiveState();
  invalidateReading(DepthQuality::STOPPED | DepthQuality::MISSING_BASELINE |
                    DepthQuality::RATE_UNAVAILABLE);
}

bool DepthSensor::due(uint64_t timestampUs) const {
  if (lastSampleUs_ == 0) return true;
  return timestampUs - lastSampleUs_ >=
         static_cast<uint64_t>(config_.samplePeriodMs) * 1000ULL;
}

bool DepthSensor::update() {
  const uint64_t timestampUs = nowUs();
  lastSampleUs_ = timestampUs;
  if (!ready_) {
    invalidateReading(DepthQuality::I2C_ERROR | DepthQuality::SENSOR_INVALID |
                      (diveActive_ ? 0 : DepthQuality::MISSING_BASELINE));
    return false;
  }

  float pressurePa = NAN;
  float temperatureC = NAN;
  DepthQualityFlags failureFlags = DepthQuality::SENSOR_INVALID;
  if (!readPressure(pressurePa, temperatureC, failureFlags)) {
    ready_ = false;
    diveActive_ = false;
    surfacePressurePa_ = NAN;
    resetDiveState();
    invalidateReading(timestampUs, pressurePa, temperatureC,
                      failureFlags | DepthQuality::SENSOR_INVALID |
                      DepthQuality::MISSING_BASELINE);
    return false;
  }

  lastValidSampleUs_ = timestampUs;
  processSample(timestampUs, pressurePa, temperatureC);
  return reading_.sensorValid;
}

DepthReading DepthSensor::snapshot(uint64_t timestampUs) const {
  DepthReading result = reading_;
  if (isStale(timestampUs)) {
    result.qualityFlags |= DepthQuality::STALE;
    result.sensorValid = false;
    result.depthValid = false;
    result.rateValid = false;
  }
  return result;
}

bool DepthSensor::isStale(uint64_t timestampUs) const {
  if (lastValidSampleUs_ == 0) return true;
  return timestampUs - lastValidSampleUs_ >=
         static_cast<uint64_t>(config_.staleAfterMs) * 1000ULL;
}

bool DepthSensor::sensorPresent() const {
  if (!bus_) return false;
  bus_->beginTransmission(address_);
  return bus_->endTransmission() == 0;
}

bool DepthSensor::readPressure(float& pressurePa, float& temperatureC,
                               DepthQualityFlags& failureFlags) {
  failureFlags = DepthQuality::SENSOR_INVALID;
  if (!sensorPresent()) {
    failureFlags |= DepthQuality::I2C_ERROR;
    return false;
  }

  // The official library performs the D1 and D2 conversions and applies the
  // MS5837 temperature compensation internally. It exposes no transaction
  // status from read(), so the ACK/range checks are necessarily partial.
  driver_.read();
  pressurePa = driver_.pressure(MS5837::Pa);
  temperatureC = driver_.temperature();
  if (!sensorPresent()) {
    failureFlags |= DepthQuality::I2C_ERROR;
    return false;
  }

  const bool valid = std::isfinite(pressurePa) &&
                     pressurePa > config_.minimumPressurePa &&
                     pressurePa <= config_.maximumPressurePa &&
                     std::isfinite(temperatureC) &&
                     temperatureC >= config_.minimumTemperatureC &&
                     temperatureC <= config_.maximumTemperatureC;
  if (!valid) return false;
  failureFlags = DepthQuality::NONE;
  return true;
}

void DepthSensor::processSample(uint64_t timestampUs, float pressurePa,
                                float temperatureC) {
  reading_ = DepthReading{};
  reading_.timestampUs = timestampUs;
  reading_.pressurePa = pressurePa;
  reading_.temperatureC = temperatureC;
  reading_.surfacePressurePa = surfacePressurePa_;
  reading_.sensorValid = true;
  reading_.qualityFlags = DepthQuality::RATE_UNAVAILABLE;

  if (!diveActive_ || !std::isfinite(surfacePressurePa_)) {
    reading_.qualityFlags |= DepthQuality::MISSING_BASELINE;
    return;
  }

  if (!std::isfinite(config_.waterDensityKgM3) ||
      config_.waterDensityKgM3 <= 0.0f) {
    reading_.sensorValid = false;
    reading_.qualityFlags |= DepthQuality::SENSOR_INVALID;
    return;
  }

  const float rawDepthM = (pressurePa - surfacePressurePa_) /
                          (config_.waterDensityKgM3 * GRAVITY_MPS2);
  if (!std::isfinite(rawDepthM)) {
    reading_.sensorValid = false;
    reading_.qualityFlags |= DepthQuality::SENSOR_INVALID;
    return;
  }
  reading_.rawDepthM = rawDepthM;

  bool warming = false;
  if (isHampelOutlier(rawDepthM, warming)) {
    reading_.qualityFlags |= DepthQuality::OUTLIER_REJECTED;
    reading_.depthValid = false;
    return;
  }
  if (warming) reading_.qualityFlags |= DepthQuality::FILTER_WARMUP;

  appendRawHistory(rawDepthM, timestampUs);
  const float filteredDepthM = filteredDepthValid_
      ? config_.emaAlpha * rawDepthM +
            (1.0f - config_.emaAlpha) * previousFilteredDepthM_
      : rawDepthM;
  if (!std::isfinite(filteredDepthM)) {
    reading_.qualityFlags |= DepthQuality::SENSOR_INVALID;
    return;
  }

  previousFilteredDepthM_ = filteredDepthM;
  filteredDepthValid_ = true;
  appendFilteredHistory(filteredDepthM, timestampUs);
  reading_.filteredDepthM = filteredDepthM;
  reading_.depthValid = true;
  updateDepthRate(timestampUs);
}

void DepthSensor::updateDepthRate(uint64_t timestampUs) {
  reading_.depthRateMps = NAN;
  reading_.rateValid = false;
  reading_.qualityFlags |= DepthQuality::RATE_UNAVAILABLE;

  if (config_.minimumRegressionSamples < 2 || filteredHistoryCount_ == 0)
    return;

  const uint64_t windowUs = static_cast<uint64_t>(config_.regressionWindowMs) * 1000ULL;
  const uint64_t firstAllowedUs = timestampUs > windowUs ? timestampUs - windowUs : 0;
  size_t sampleCount = 0;
  uint64_t firstTimestampUs = 0;
  for (size_t i = 0; i < filteredHistoryCount_; ++i) {
    const HistorySample& sample = filteredHistory_[i];
    if (sample.valid && sample.timestampUs >= firstAllowedUs &&
        sample.timestampUs <= timestampUs) {
      if (sampleCount == 0) firstTimestampUs = sample.timestampUs;
      ++sampleCount;
    }
  }
  if (sampleCount < config_.minimumRegressionSamples) return;

  double timeSum = 0.0;
  double depthSum = 0.0;
  for (size_t i = 0; i < filteredHistoryCount_; ++i) {
    const HistorySample& sample = filteredHistory_[i];
    if (!sample.valid || sample.timestampUs < firstAllowedUs ||
        sample.timestampUs > timestampUs) continue;
    const double timeSeconds = static_cast<double>(sample.timestampUs - firstTimestampUs) / 1000000.0;
    timeSum += timeSeconds;
    depthSum += sample.depthM;
  }
  const double meanTime = timeSum / static_cast<double>(sampleCount);
  const double meanDepth = depthSum / static_cast<double>(sampleCount);

  double numerator = 0.0;
  double denominator = 0.0;
  for (size_t i = 0; i < filteredHistoryCount_; ++i) {
    const HistorySample& sample = filteredHistory_[i];
    if (!sample.valid || sample.timestampUs < firstAllowedUs ||
        sample.timestampUs > timestampUs) continue;
    const double timeSeconds = static_cast<double>(sample.timestampUs - firstTimestampUs) / 1000000.0;
    const double timeDelta = timeSeconds - meanTime;
    const double depthDelta = static_cast<double>(sample.depthM) - meanDepth;
    numerator += timeDelta * depthDelta;
    denominator += timeDelta * timeDelta;
  }
  if (denominator <= 1e-12) return;

  const float rate = static_cast<float>(numerator / denominator);
  if (!std::isfinite(rate)) return;
  reading_.depthRateMps = rate;
  reading_.rateValid = true;
  reading_.qualityFlags &= static_cast<DepthQualityFlags>(~DepthQuality::RATE_UNAVAILABLE);
}

bool DepthSensor::isHampelOutlier(float depthM, bool& warming) {
  warming = false;
  const size_t configuredWindow = std::min<size_t>(
      std::max<size_t>(config_.hampelWindowSamples, 3), HISTORY_CAPACITY);
  const size_t requiredPrevious = configuredWindow - 1;
  const size_t previousCount = std::min(rawHistoryCount_, requiredPrevious);
  if (previousCount < 2) {
    warming = true;
    return false;
  }

  float values[HISTORY_CAPACITY] = {};
  for (size_t i = 0; i < previousCount; ++i)
    values[i] = rawHistory_[rawHistoryCount_ - previousCount + i].depthM;
  const float neighborhoodMedian = median(values, previousCount);

  float deviations[HISTORY_CAPACITY] = {};
  for (size_t i = 0; i < previousCount; ++i)
    deviations[i] = std::fabs(values[i] - neighborhoodMedian);
  const float mad = median(deviations, previousCount);
  const float allowedDeviation = std::max(
      config_.hampelMinimumDeviationM,
      config_.hampelDeviationThreshold * MAD_TO_STANDARD_DEVIATION * mad);
  return std::fabs(depthM - neighborhoodMedian) > allowedDeviation;
}

void DepthSensor::appendRawHistory(float depthM, uint64_t timestampUs) {
  if (rawHistoryCount_ == HISTORY_CAPACITY) {
    for (size_t i = 1; i < HISTORY_CAPACITY; ++i)
      rawHistory_[i - 1] = rawHistory_[i];
    --rawHistoryCount_;
  }
  rawHistory_[rawHistoryCount_++] = {timestampUs, depthM, true};
}

void DepthSensor::appendFilteredHistory(float depthM, uint64_t timestampUs) {
  if (filteredHistoryCount_ == HISTORY_CAPACITY) {
    for (size_t i = 1; i < HISTORY_CAPACITY; ++i)
      filteredHistory_[i - 1] = filteredHistory_[i];
    --filteredHistoryCount_;
  }
  filteredHistory_[filteredHistoryCount_++] = {timestampUs, depthM, true};
}

void DepthSensor::invalidateReading(DepthQualityFlags flags) {
  invalidateReading(0, NAN, NAN, flags);
}

void DepthSensor::invalidateReading(uint64_t timestampUs, float pressurePa,
                                    float temperatureC,
                                    DepthQualityFlags flags) {
  reading_ = DepthReading{};
  reading_.timestampUs = timestampUs;
  reading_.pressurePa = pressurePa;
  reading_.temperatureC = temperatureC;
  reading_.surfacePressurePa = surfacePressurePa_;
  reading_.qualityFlags |= flags;
}

void DepthSensor::resetDiveState() {
  diveActive_ = false;
  surfacePressurePa_ = NAN;
  previousFilteredDepthM_ = NAN;
  filteredDepthValid_ = false;
  lastSampleUs_ = 0;
  rawHistoryCount_ = 0;
  filteredHistoryCount_ = 0;
  lastValidSampleUs_ = 0;
  reading_ = DepthReading{};
}
