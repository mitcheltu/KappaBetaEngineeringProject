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

uint64_t monotonicNowUs() {
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
  resetDiveState();
  zeroPending_ = true;
  return true;
}

void DepthSensor::stopDive() {
  conversionState_ = ConversionState::IDLE;
  conversionDeadlineUs_ = 0;
  zeroPending_ = false;
  lastSampleUs_ = 0;
  sampleClockValid_ = false;
  resetDiveState();
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
      processSample(timestampUs, pressurePa, temperatureC);
      if (!reading_.sensorValid) {
        return failConversion(timestampUs, DepthQuality::SENSOR_INVALID);
      }
      conversionState_ = ConversionState::IDLE;
      conversionDeadlineUs_ = 0;
      return surfaceZero ? DepthServiceEvent::SURFACE_ZERO_PUBLISHED
                         : DepthServiceEvent::SAMPLE_PUBLISHED;
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
  ready_ = false;
  conversionState_ = ConversionState::IDLE;
  conversionDeadlineUs_ = 0;
  zeroPending_ = false;
  resetDiveState();
  invalidateReading(timestampUs, NAN, NAN,
                    flags | DepthQuality::MISSING_BASELINE);
  return surfaceZero ? DepthServiceEvent::SURFACE_ZERO_FAILED
                     : DepthServiceEvent::CONVERSION_FAILED;
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
  rawHistoryCount_ = 0;
  filteredHistoryCount_ = 0;
  lastValidSampleUs_ = 0;
  reading_ = DepthReading{};
}
