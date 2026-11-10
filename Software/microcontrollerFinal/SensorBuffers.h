#pragma once

#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "DepthSensor.h"
#include "FirmwareConfig.h"
#include "ImuSensor.h"
#include "PpgSensor.h"

namespace SensorBuffersDetail {

template <typename Reading, std::size_t Capacity>
class TimestampedRing {
 public:
  void push(const Reading& reading) {
    entries_[nextWrite_] = reading;
    nextWrite_ = (nextWrite_ + 1) % Capacity;
    if (count_ < Capacity) ++count_;
  }

  std::size_t size() const { return count_; }

  std::size_t copyRange(uint64_t startUs, uint64_t endUs, Reading* output,
                        std::size_t outputCapacity) const {
    if (!output || outputCapacity == 0 || endUs < startUs) return 0;
    std::size_t matching = 0;
    visitChronological([&](const Reading& item) {
      if (item.timestampUs >= startUs && item.timestampUs <= endUs) ++matching;
    });

    std::size_t skip = matching > outputCapacity ? matching - outputCapacity : 0;
    std::size_t copied = 0;
    visitChronological([&](const Reading& item) {
      if (item.timestampUs < startUs || item.timestampUs > endUs) return;
      if (skip != 0) {
        --skip;
        return;
      }
      if (copied < outputCapacity) output[copied++] = item;
    });
    return copied;
  }

  std::size_t copyLatest(uint64_t endUs, Reading* output,
                         std::size_t outputCapacity) const {
    if (!output || outputCapacity == 0) return 0;
    std::size_t eligible = 0;
    visitChronological([&](const Reading& item) {
      if (item.timestampUs <= endUs) ++eligible;
    });

    std::size_t skip = eligible > outputCapacity ? eligible - outputCapacity : 0;
    std::size_t copied = 0;
    visitChronological([&](const Reading& item) {
      if (item.timestampUs > endUs) return;
      if (skip != 0) {
        --skip;
        return;
      }
      if (copied < outputCapacity) output[copied++] = item;
    });
    return copied;
  }

 private:
  template <typename Visitor>
  void visitChronological(Visitor visitor) const {
    const std::size_t oldest = (nextWrite_ + Capacity - count_) % Capacity;
    for (std::size_t offset = 0; offset < count_; ++offset)
      visitor(entries_[(oldest + offset) % Capacity]);
  }

  Reading entries_[Capacity]{};
  std::size_t nextWrite_ = 0;
  std::size_t count_ = 0;
};

} // namespace SensorBuffersDetail

// Acquisition is the only writer. Readers copy under a short mutex and then
// process their private snapshots after the mutex has been released.
class SensorBuffers {
 public:
  bool begin() {
    if (mutex_) return true;
    mutex_ = xSemaphoreCreateMutex();
    return mutex_ != nullptr;
  }

  bool pushImu(const ImuReading& reading) {
    if (!lock()) return false;
    imu_.push(reading);
    unlock();
    return true;
  }

  bool pushDepth(const DepthReading& reading) {
    if (!lock()) return false;
    depth_.push(reading);
    unlock();
    return true;
  }

  bool pushPpg(const PpgReading& reading) {
    if (!lock()) return false;
    ppg_.push(reading);
    unlock();
    return true;
  }

  bool publishPpgAnalysis(const PpgAnalysis& analysis) {
    if (!lock()) return false;
    ppgAnalysis_ = analysis;
    unlock();
    return true;
  }

  std::size_t imuCount() const {
    if (!lock()) return 0;
    const std::size_t count = imu_.size();
    unlock();
    return count;
  }

  std::size_t copyImuRange(uint64_t startUs, uint64_t endUs,
                           ImuReading* output, std::size_t capacity) const {
    if (!lock()) return 0;
    const std::size_t copied = imu_.copyRange(startUs, endUs, output, capacity);
    unlock();
    return copied;
  }

  std::size_t copyLatestImu(uint64_t endUs, ImuReading* output,
                            std::size_t capacity) const {
    if (!lock()) return 0;
    const std::size_t copied = imu_.copyLatest(endUs, output, capacity);
    unlock();
    return copied;
  }

  bool latestImu(uint64_t endUs, ImuReading& output) const {
    return copyLatestImu(endUs, &output, 1) == 1;
  }

  std::size_t copyDepthRange(uint64_t startUs, uint64_t endUs,
                             DepthReading* output, std::size_t capacity) const {
    if (!lock()) return 0;
    const std::size_t copied = depth_.copyRange(startUs, endUs, output, capacity);
    unlock();
    return copied;
  }

  std::size_t copyLatestDepth(uint64_t endUs, DepthReading* output,
                              std::size_t capacity) const {
    if (!lock()) return 0;
    const std::size_t copied = depth_.copyLatest(endUs, output, capacity);
    unlock();
    return copied;
  }

  bool latestDepth(uint64_t endUs, DepthReading& output) const {
    return copyLatestDepth(endUs, &output, 1) == 1;
  }

  std::size_t copyPpgRange(uint64_t startUs, uint64_t endUs,
                           PpgReading* output, std::size_t capacity) const {
    if (!lock()) return 0;
    const std::size_t copied = ppg_.copyRange(startUs, endUs, output, capacity);
    unlock();
    return copied;
  }

  std::size_t copyLatestPpg(uint64_t endUs, PpgReading* output,
                            std::size_t capacity) const {
    if (!lock()) return 0;
    const std::size_t copied = ppg_.copyLatest(endUs, output, capacity);
    unlock();
    return copied;
  }

  bool latestPpg(uint64_t endUs, PpgReading& output) const {
    return copyLatestPpg(endUs, &output, 1) == 1;
  }

  bool latestPpgAnalysis(uint64_t endUs, PpgAnalysis& output) const {
    if (!lock()) return false;
    const bool available = ppgAnalysis_.timestampUs != 0 &&
                           ppgAnalysis_.timestampUs <= endUs;
    if (available) output = ppgAnalysis_;
    unlock();
    return available;
  }

 private:
  bool lock() const {
    return mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE;
  }

  void unlock() const { xSemaphoreGive(mutex_); }

  mutable SemaphoreHandle_t mutex_ = nullptr;
  SensorBuffersDetail::TimestampedRing<ImuReading, Config::IMU_BUFFER_CAPACITY> imu_{};
  SensorBuffersDetail::TimestampedRing<DepthReading, Config::DEPTH_BUFFER_CAPACITY> depth_{};
  SensorBuffersDetail::TimestampedRing<PpgReading, Config::PPG_BUFFER_CAPACITY> ppg_{};
  PpgAnalysis ppgAnalysis_{};
};
