#include "DepthSensor.h"

#include <algorithm>
#include <esp_timer.h>

namespace {
constexpr uint8_t MS5837_ADDRESS = 0x76;
constexpr uint8_t MS5837_RESET = 0x1E;
constexpr uint8_t MS5837_ADC_READ = 0x00;
constexpr uint8_t MS5837_PROM_READ = 0xA0;
constexpr uint8_t MS5837_CONVERT_D1_8192 = 0x4A;
constexpr uint8_t MS5837_CONVERT_D2_8192 = 0x5A;
constexpr float GRAVITY_MPS2 = 9.80665f;
constexpr float MAD_TO_STANDARD_DEVIATION = 1.4826f;

struct WeightedValue {
  float value = NAN;
  uint8_t weight = 1;
};

uint64_t monotonicNowUs() {
  return static_cast<uint64_t>(esp_timer_get_time());
}

float weightedValueAtRank(const WeightedValue* values, size_t count,
                          size_t rank) {
  size_t cumulativeWeight = 0;
  for (size_t i = 0; i < count; ++i) {
    cumulativeWeight += values[i].weight;
    if (rank < cumulativeWeight) return values[i].value;
  }
  return NAN;
}

float weightedMedian(WeightedValue* values, size_t count) {
  if (count == 0) return NAN;
  std::sort(values, values + count,
            [](const WeightedValue& left, const WeightedValue& right) {
              return left.value < right.value;
            });

  size_t totalWeight = 0;
  for (size_t i = 0; i < count; ++i) totalWeight += values[i].weight;
  if (totalWeight == 0) return NAN;
  if ((totalWeight & 1u) != 0u)
    return weightedValueAtRank(values, count, totalWeight / 2u);

  const float lower = weightedValueAtRank(values, count, totalWeight / 2u - 1u);
  const float upper = weightedValueAtRank(values, count, totalWeight / 2u);
  return (lower + upper) * 0.5f;
}
} // namespace

DepthSensor::DepthSensor(const DepthSensorConfig& config) : config_(config) {
  resetDiveState();
}

bool DepthSensor::begin(TwoWire& bus, uint8_t address) {
  flushPendingAsUnclassifiable();
  ready_ = false;
  bus_ = &bus;
  address_ = address;
  conversionState_ = ConversionState::IDLE;
  conversionDeadlineUs_ = 0;
  zeroPending_ = false;
  lastSampleUs_ = 0;
  sampleClockValid_ = false;
  resetDiveState();

  // The pinned BlueRobotics 1.1.1 library uses the global Wire object and
  // hardcodes 0x76. Keep the same hardware contract for the async path.
  if (bus_ != &Wire || address_ != MS5837_ADDRESS) {
    invalidateReading(DepthQuality::I2C_ERROR | DepthQuality::SENSOR_INVALID);
    return false;
  }

  if (!sensorPresent() || !writeCommand(MS5837_RESET)) {
    invalidateReading(DepthQuality::I2C_ERROR | DepthQuality::SENSOR_INVALID);
    return false;
  }

  delay(10);
  if (!loadCalibration()) {
    invalidateReading(DepthQuality::I2C_ERROR | DepthQuality::SENSOR_INVALID);
    return false;
  }
  ready_ = true;
  invalidateReading(DepthQuality::MISSING_BASELINE | DepthQuality::RATE_UNAVAILABLE);
  return true;
}

bool DepthSensor::startDive() {
  if (!ready_) {
    invalidateReading(DepthQuality::I2C_ERROR | DepthQuality::SENSOR_INVALID |
                      DepthQuality::MISSING_BASELINE);
    return false;
  }

  // Clear the old baseline immediately, but take the fresh zero through the
  // same D1/D2 state machine as normal samples. If a conversion is in flight,
  // it is allowed to finish and the queued zero starts on the next idle pass.
  flushPendingAsUnclassifiable();
  resetDiveState();
  queueSegmentBoundary(DepthQuality::FILTER_WARMUP);
  zeroPending_ = true;
  return true;
}

void DepthSensor::stopDive() {
  flushPendingAsUnclassifiable();
  conversionState_ = ConversionState::IDLE;
  conversionDeadlineUs_ = 0;
  zeroPending_ = false;
  lastSampleUs_ = 0;
  sampleClockValid_ = false;
  resetDiveState();
  queueSegmentBoundary(DepthQuality::STOPPED);
  invalidateReading(DepthQuality::STOPPED | DepthQuality::MISSING_BASELINE |
                    DepthQuality::RATE_UNAVAILABLE);
}

bool DepthSensor::due(uint64_t timestampUs) const {
  if (conversionState_ != ConversionState::IDLE || zeroPending_) return false;
  if (!sampleClockValid_) return true;
  return timestampUs - lastSampleUs_ >=
         static_cast<uint64_t>(config_.samplePeriodMs) * 1000ULL;
}

bool DepthSensor::update() {
  const DepthServiceEvent event = service();
  return event == DepthServiceEvent::SAMPLE_PUBLISHED ||
         event == DepthServiceEvent::SURFACE_ZERO_PUBLISHED;
}

DepthServiceEvent DepthSensor::service() {
  const DepthServiceEvent queuedOutput = drainPublishedOutput();
  if (queuedOutput != DepthServiceEvent::NONE) return queuedOutput;

  const uint64_t timestampUs = monotonicNowUs();

  switch (conversionState_) {
    case ConversionState::IDLE: {
      if (!ready_) return DepthServiceEvent::NONE;

      if (zeroPending_) {
        zeroPending_ = false;
        conversionKind_ = ConversionKind::SURFACE_ZERO;
      } else if (!due(timestampUs)) {
        return DepthServiceEvent::NONE;
      } else {
        conversionKind_ = ConversionKind::NORMAL_SAMPLE;
      }

      lastSampleUs_ = timestampUs;
      sampleClockValid_ = true;
      if (!startConversion(MS5837_CONVERT_D1_8192, timestampUs)) {
        return failConversion(timestampUs, DepthQuality::I2C_ERROR |
                                           DepthQuality::SENSOR_INVALID);
      }
      conversionState_ = ConversionState::WAITING_PRESSURE;
      return DepthServiceEvent::NONE;
    }

    case ConversionState::WAITING_PRESSURE:
      if (timestampUs < conversionDeadlineUs_) return DepthServiceEvent::NONE;
      if (!readAdc(pressureAdc_)) {
        return failConversion(timestampUs, DepthQuality::I2C_ERROR |
                                           DepthQuality::SENSOR_INVALID);
      }
      if (!startConversion(MS5837_CONVERT_D2_8192, timestampUs)) {
        return failConversion(timestampUs, DepthQuality::I2C_ERROR |
                                           DepthQuality::SENSOR_INVALID);
      }
      conversionState_ = ConversionState::WAITING_TEMPERATURE;
      return DepthServiceEvent::NONE;

    case ConversionState::WAITING_TEMPERATURE: {
      if (timestampUs < conversionDeadlineUs_) return DepthServiceEvent::NONE;

      uint32_t temperatureAdc = 0;
      if (!readAdc(temperatureAdc)) {
        return failConversion(timestampUs, DepthQuality::I2C_ERROR |
                                           DepthQuality::SENSOR_INVALID);
      }

      float pressurePa = NAN;
      float temperatureC = NAN;
      if (!compensate(pressureAdc_, temperatureAdc, pressurePa, temperatureC)) {
        return failConversion(timestampUs, DepthQuality::SENSOR_INVALID);
      }

      const bool valid = std::isfinite(pressurePa) &&
                         pressurePa > config_.minimumPressurePa &&
                         pressurePa <= config_.maximumPressurePa &&
                         std::isfinite(temperatureC) &&
                         temperatureC >= config_.minimumTemperatureC &&
                         temperatureC <= config_.maximumTemperatureC;
      if (!valid) {
        return failConversion(timestampUs, DepthQuality::SENSOR_INVALID);
      }

      const bool surfaceZero = conversionKind_ == ConversionKind::SURFACE_ZERO;
      if (surfaceZero) {
        surfacePressurePa_ = pressurePa;
        diveActive_ = true;
      }
      lastValidSampleUs_ = timestampUs;
      bool sampleValid = false;
      const bool samplePublished = processSample(
          timestampUs, pressurePa, temperatureC, surfaceZero, sampleValid);
      if (!sampleValid) {
        return failConversion(timestampUs, DepthQuality::SENSOR_INVALID);
      }
      conversionState_ = ConversionState::IDLE;
      conversionDeadlineUs_ = 0;
      if (surfaceZero) return DepthServiceEvent::SURFACE_ZERO_PUBLISHED;
      return samplePublished ? DepthServiceEvent::SAMPLE_PUBLISHED
                             : DepthServiceEvent::NONE;
    }
  }

  return DepthServiceEvent::NONE;
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

uint64_t DepthSensor::finalizedThroughUs(uint64_t nowUs) const {
  uint64_t finalizedThroughUs = nowUs;
  const auto limitBefore = [](uint64_t timestampUs) {
    return timestampUs == 0 ? 0 : timestampUs - 1;
  };

  if (pendingSampleCount_ != 0)
    finalizedThroughUs = std::min(
        finalizedThroughUs, limitBefore(pendingSamples_[0].timestampUs));
  if (publishedReadingCount_ != 0)
    finalizedThroughUs = std::min(
        finalizedThroughUs, limitBefore(publishedReadings_[0].timestampUs));
  if (deferredFailureEvent_ != DepthServiceEvent::NONE)
    finalizedThroughUs = std::min(
        finalizedThroughUs, limitBefore(deferredFailureReading_.timestampUs));
  return finalizedThroughUs;
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

bool DepthSensor::writeCommand(uint8_t command) {
  if (!bus_) return false;
  bus_->beginTransmission(address_);
  if (bus_->write(command) != 1) return false;
  return bus_->endTransmission() == 0;
}

bool DepthSensor::readAdc(uint32_t& value) {
  if (!writeCommand(MS5837_ADC_READ)) return false;

  constexpr size_t ADC_BYTES = 3;
  if (bus_->requestFrom(address_, static_cast<uint8_t>(ADC_BYTES)) != ADC_BYTES ||
      bus_->available() != static_cast<int>(ADC_BYTES)) {
    return false;
  }

  const int first = bus_->read();
  const int second = bus_->read();
  const int third = bus_->read();
  if (first < 0 || second < 0 || third < 0) return false;

  value = (static_cast<uint32_t>(first) << 16) |
          (static_cast<uint32_t>(second) << 8) |
          static_cast<uint32_t>(third);
  return true;
}

bool DepthSensor::readCalibrationWord(uint8_t index, uint16_t& value) {
  if (index >= 7 || !writeCommand(static_cast<uint8_t>(MS5837_PROM_READ + index * 2)))
    return false;

  constexpr size_t PROM_BYTES = 2;
  if (bus_->requestFrom(address_, static_cast<uint8_t>(PROM_BYTES)) != PROM_BYTES ||
      bus_->available() != static_cast<int>(PROM_BYTES)) {
    return false;
  }

  const int high = bus_->read();
  const int low = bus_->read();
  if (high < 0 || low < 0) return false;
  value = static_cast<uint16_t>((high << 8) | low);
  return true;
}

bool DepthSensor::loadCalibration() {
  uint16_t prom[8] = {};
  for (uint8_t index = 0; index < 7; ++index) {
    if (!readCalibrationWord(index, prom[index])) return false;
  }

  const uint8_t crcRead = static_cast<uint8_t>(prom[0] >> 12);
  const uint8_t crcCalculated = crc4(prom);
  if (crcCalculated != crcRead) return false;

  for (size_t index = 0; index < 8; ++index) calibration_[index] = prom[index];
  return true;
}

uint8_t DepthSensor::crc4(uint16_t prom[8]) const {
  // Same CRC-4 algorithm as BlueRobotics MS5837 v1.1.1::crc4().
  uint16_t remainder = 0;
  prom[0] = static_cast<uint16_t>(prom[0] & 0x0FFF);
  prom[7] = 0;

  for (uint8_t index = 0; index < 16; ++index) {
    if ((index & 1u) != 0u) {
      remainder ^= static_cast<uint16_t>(prom[index >> 1] & 0x00FF);
    } else {
      remainder ^= static_cast<uint16_t>(prom[index >> 1] >> 8);
    }
    for (uint8_t bit = 8; bit > 0; --bit) {
      remainder = (remainder & 0x8000) != 0
          ? static_cast<uint16_t>((remainder << 1) ^ 0x3000)
          : static_cast<uint16_t>(remainder << 1);
    }
  }

  return static_cast<uint8_t>((remainder >> 12) & 0x000F);
}

bool DepthSensor::startConversion(uint8_t command, uint64_t timestampUs) {
  if (!writeCommand(command)) return false;
  conversionDeadlineUs_ = timestampUs + CONVERSION_TIME_US;
  return true;
}

bool DepthSensor::compensate(uint32_t pressureAdc, uint32_t temperatureAdc,
                             float& pressurePa, float& temperatureC) const {
  // Same 30BA first- and second-order compensation as the pinned library.
  const int32_t dT = static_cast<int32_t>(
      temperatureAdc - static_cast<uint32_t>(calibration_[5]) * 256UL);
  const int64_t sens = static_cast<int64_t>(calibration_[1]) * 32768L +
                       (static_cast<int64_t>(calibration_[3]) * dT) / 256L;
  const int64_t off = static_cast<int64_t>(calibration_[2]) * 65536L +
                      (static_cast<int64_t>(calibration_[4]) * dT) / 128L;
  const int32_t temp = static_cast<int32_t>(
      2000L + static_cast<int64_t>(dT) * calibration_[6] / 8388608LL);

  int32_t ti = 0;
  int32_t offi = 0;
  int32_t sensi = 0;
  if ((temp / 100) < 20) {
    ti = static_cast<int32_t>((3 * static_cast<int64_t>(dT) * dT) /
                              8589934592LL);
    offi = static_cast<int32_t>((3 * static_cast<int64_t>(temp - 2000) *
                                 (temp - 2000)) / 2);
    sensi = static_cast<int32_t>((5 * static_cast<int64_t>(temp - 2000) *
                                  (temp - 2000)) / 8);
    if ((temp / 100) < -15) {
      offi += 7 * (temp + 1500L) * (temp + 1500L);
      sensi += 4 * (temp + 1500L) * (temp + 1500L);
    }
  } else if ((temp / 100) >= 20) {
    ti = static_cast<int32_t>((2 * static_cast<int64_t>(dT) * dT) /
                              137438953472LL);
    offi = static_cast<int32_t>((temp - 2000) * (temp - 2000) / 16);
    sensi = 0;
  }

  const int64_t off2 = off - offi;
  const int64_t sens2 = sens - sensi;
  const int32_t compensatedTemp = temp - ti;
  const int64_t pressure =
      (static_cast<int64_t>(pressureAdc) * sens2 / 2097152L - off2) / 8192L;

  pressurePa = static_cast<float>(pressure) * 10.0f;
  temperatureC = static_cast<float>(compensatedTemp) / 100.0f;
  return std::isfinite(pressurePa) && std::isfinite(temperatureC);
}

DepthServiceEvent DepthSensor::failConversion(uint64_t timestampUs,
                                              DepthQualityFlags flags) {
  const bool surfaceZero = conversionKind_ == ConversionKind::SURFACE_ZERO;
  flushPendingAsUnclassifiable();
  ready_ = false;
  conversionState_ = ConversionState::IDLE;
  conversionDeadlineUs_ = 0;
  zeroPending_ = false;
  resetDiveState();
  invalidateReading(timestampUs, NAN, NAN,
                    flags | DepthQuality::MISSING_BASELINE);
  const DepthServiceEvent failureEvent = surfaceZero
      ? DepthServiceEvent::SURFACE_ZERO_FAILED
      : DepthServiceEvent::CONVERSION_FAILED;
  if (publishedReadingCount_ != 0) {
    deferredFailureEvent_ = failureEvent;
    deferredFailureReading_ = reading_;
    return drainPublishedOutput();
  }
  return failureEvent;
}

bool DepthSensor::processSample(uint64_t timestampUs, float pressurePa,
                                float temperatureC, bool surfaceZero,
                                bool& sampleValid) {
  sampleValid = true;
  DepthReading sample{};
  sample.timestampUs = timestampUs;
  sample.pressurePa = pressurePa;
  sample.temperatureC = temperatureC;
  sample.surfacePressurePa = surfacePressurePa_;
  sample.sensorValid = true;
  sample.qualityFlags = DepthQuality::RATE_UNAVAILABLE;

  if (!diveActive_ || !std::isfinite(surfacePressurePa_)) {
    sample.qualityFlags |= DepthQuality::MISSING_BASELINE;
    reading_ = sample;
    return true;
  }

  if (!std::isfinite(config_.waterDensityKgM3) ||
      config_.waterDensityKgM3 <= 0.0f) {
    sample.sensorValid = false;
    sample.qualityFlags |= DepthQuality::SENSOR_INVALID;
    reading_ = sample;
    sampleValid = false;
    return false;
  }

  const float rawDepthM = (pressurePa - surfacePressurePa_) /
                          (config_.waterDensityKgM3 * GRAVITY_MPS2);
  if (!std::isfinite(rawDepthM)) {
    sample.sensorValid = false;
    sample.qualityFlags |= DepthQuality::SENSOR_INVALID;
    reading_ = sample;
    sampleValid = false;
    return false;
  }
  sample.rawDepthM = rawDepthM;
  // Keep every finite raw depth, including future outliers, available to later
  // windows before any candidate is classified.
  appendRawHistory(rawDepthM, timestampUs);

  if (surfaceZero) {
    // The calibration point is accepted by definition and starts a new filter
    // segment; later normal samples use it as their first historical value.
    sample.qualityFlags |= DepthQuality::FILTER_WARMUP;
    sample.filteredDepthM = rawDepthM;
    sample.depthValid = true;
    previousFilteredDepthM_ = rawDepthM;
    filteredDepthValid_ = true;
    appendFilteredHistory(rawDepthM, timestampUs);
    reading_ = sample;
    updateDepthRate(timestampUs);
    return true;
  }

  if (pendingSampleCount_ >= PENDING_CAPACITY) {
    // This cannot occur in normal operation: one new raw reading makes the
    // oldest candidate ready before another conversion can publish a sample.
    // Treat an unexpected queue overflow as a conversion failure rather than
    // silently losing chronological samples.
    sampleValid = false;
    return false;
  }
  pendingSamples_[pendingSampleCount_++] = sample;
  if (pendingSampleCount_ <= Config::DEPTH_HAMPEL_FUTURE_READINGS)
    return false;
  return finalizeOldestPendingSample();
}

bool DepthSensor::finalizeOldestPendingSample() {
  if (pendingSampleCount_ <= Config::DEPTH_HAMPEL_FUTURE_READINGS)
    return false;

  DepthReading sample = pendingSamples_[0];
  for (size_t i = 1; i < pendingSampleCount_; ++i)
    pendingSamples_[i - 1] = pendingSamples_[i];
  --pendingSampleCount_;

  finalizeSample(sample);
  reading_ = sample;
  return true;
}

void DepthSensor::finalizeSample(DepthReading& sample) {
  bool warming = false;
  if (isHampelOutlier(sample, warming)) {
    sample.qualityFlags |= DepthQuality::OUTLIER_REJECTED;
    if (warming) sample.qualityFlags |= DepthQuality::FILTER_WARMUP;
    sample.depthValid = false;
    sample.rateValid = false;
    sample.filteredDepthM = NAN;
    sample.depthRateMps = NAN;
    sample.rateWindowStartUs = 0;
    sample.rateWindowEndUs = sample.timestampUs;
    sample.qualityFlags |= DepthQuality::RATE_UNAVAILABLE;
    return;
  }

  if (warming) sample.qualityFlags |= DepthQuality::FILTER_WARMUP;
  const float filteredDepthM = filteredDepthValid_
      ? config_.emaAlpha * sample.rawDepthM +
            (1.0f - config_.emaAlpha) * previousFilteredDepthM_
      : sample.rawDepthM;
  if (!std::isfinite(filteredDepthM)) {
    sample.sensorValid = false;
    sample.qualityFlags |= DepthQuality::SENSOR_INVALID;
    sample.depthValid = false;
    sample.rateValid = false;
    sample.filteredDepthM = NAN;
    sample.depthRateMps = NAN;
    sample.rateWindowStartUs = 0;
    sample.rateWindowEndUs = sample.timestampUs;
    sample.qualityFlags |= DepthQuality::RATE_UNAVAILABLE;
    return;
  }

  previousFilteredDepthM_ = filteredDepthM;
  filteredDepthValid_ = true;
  appendFilteredHistory(filteredDepthM, sample.timestampUs);
  sample.filteredDepthM = filteredDepthM;
  sample.depthValid = true;
  reading_ = sample;
  updateDepthRate(sample.timestampUs);
  sample = reading_;
}

void DepthSensor::updateDepthRate(uint64_t timestampUs) {
  reading_.depthRateMps = NAN;
  reading_.rateValid = false;
  reading_.rateWindowStartUs = 0;
  reading_.rateWindowEndUs = timestampUs;
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
  reading_.rateWindowStartUs = firstTimestampUs;

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

bool DepthSensor::isHampelOutlier(const DepthReading& sample, bool& warming) {
  warming = false;
  const size_t futureNeeded = Config::DEPTH_HAMPEL_FUTURE_READINGS;
  const size_t configuredWindow = std::min<size_t>(
      std::max<size_t>(config_.hampelWindowSamples, futureNeeded + 1),
      Config::DEPTH_HAMPEL_WINDOW_READINGS);
  const size_t requiredPrevious = configuredWindow - futureNeeded - 1;

  size_t currentIndex = rawHistoryCount_;
  for (size_t i = 0; i < rawHistoryCount_; ++i) {
    if (rawHistory_[i].timestampUs == sample.timestampUs) {
      currentIndex = i;
      break;
    }
  }
  if (currentIndex == rawHistoryCount_) {
    warming = true;
    return false;
  }

  const size_t previousCount = std::min(currentIndex, requiredPrevious);
  const size_t futureCount = std::min(
      rawHistoryCount_ - currentIndex - 1, futureNeeded);
  warming = previousCount < requiredPrevious || futureCount < futureNeeded;
  if (futureCount < futureNeeded) return false;

  WeightedValue values[Config::DEPTH_HAMPEL_WINDOW_READINGS]{};
  size_t valueCount = 0;
  const size_t firstIndex = currentIndex - previousCount;
  const size_t lastIndex = currentIndex + futureCount;
  for (size_t i = firstIndex; i <= lastIndex; ++i) {
    const bool isPrevious = i < currentIndex;
    // Give the current and future side more influence to follow sustained
    // changes while retaining older context for isolated spike detection.
    values[valueCount++] = {
      rawHistory_[i].depthM,
      static_cast<uint8_t>(isPrevious ? 1 : 2)
    };
  }

  WeightedValue deviations[Config::DEPTH_HAMPEL_WINDOW_READINGS]{};
  const float neighborhoodMedian = weightedMedian(values, valueCount);
  if (!std::isfinite(neighborhoodMedian)) {
    warming = true;
    return false;
  }
  for (size_t i = 0; i < valueCount; ++i) {
    deviations[i] = {
      std::fabs(values[i].value - neighborhoodMedian), values[i].weight
    };
  }
  const float mad = weightedMedian(deviations, valueCount);
  if (!std::isfinite(mad)) {
    warming = true;
    return false;
  }
  const float allowedDeviation = std::max(
      config_.hampelMinimumDeviationM,
      config_.hampelDeviationThreshold * MAD_TO_STANDARD_DEVIATION * mad);
  return std::fabs(sample.rawDepthM - neighborhoodMedian) > allowedDeviation;
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

void DepthSensor::flushPendingAsUnclassifiable() {
  for (size_t i = 0; i < pendingSampleCount_; ++i) {
    DepthReading sample = pendingSamples_[i];
    sample.qualityFlags |= DepthQuality::FILTER_WARMUP |
                           DepthQuality::RATE_UNAVAILABLE;
    sample.depthValid = false;
    sample.rateValid = false;
    sample.filteredDepthM = NAN;
    sample.depthRateMps = NAN;
    sample.rateWindowStartUs = 0;
    sample.rateWindowEndUs = sample.timestampUs;
    queuePublishedReading(sample);
  }
  pendingSampleCount_ = 0;
}

void DepthSensor::queueSegmentBoundary(DepthQualityFlags flags) {
  DepthReading boundary{};
  boundary.timestampUs = monotonicNowUs();
  boundary.qualityFlags = flags | DepthQuality::MISSING_BASELINE |
                          DepthQuality::RATE_UNAVAILABLE;
  queuePublishedReading(boundary);
}

void DepthSensor::queuePublishedReading(const DepthReading& sample) {
  if (publishedReadingCount_ >= PUBLISHED_CAPACITY) return;
  publishedReadings_[publishedReadingCount_++] = sample;
}

DepthServiceEvent DepthSensor::drainPublishedOutput() {
  if (publishedReadingCount_ != 0) {
    reading_ = publishedReadings_[0];
    for (size_t i = 1; i < publishedReadingCount_; ++i)
      publishedReadings_[i - 1] = publishedReadings_[i];
    --publishedReadingCount_;
    return DepthServiceEvent::SAMPLE_PUBLISHED;
  }

  if (deferredFailureEvent_ != DepthServiceEvent::NONE) {
    reading_ = deferredFailureReading_;
    const DepthServiceEvent event = deferredFailureEvent_;
    deferredFailureEvent_ = DepthServiceEvent::NONE;
    deferredFailureReading_ = DepthReading{};
    return event;
  }
  return DepthServiceEvent::NONE;
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
  rawHistoryCount_ = 0;
  filteredHistoryCount_ = 0;
  pendingSampleCount_ = 0;
  lastValidSampleUs_ = 0;
  reading_ = DepthReading{};
}
