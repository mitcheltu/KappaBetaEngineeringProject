#pragma once
#include "HistoryProtocol.h"
#include <new>

namespace History {
// Single-owner fixed ring. The caller supplies and retains the storage.
class Buffer {
public:
  bool reset(Record* storage, uint32_t capacity, uint32_t firstSequence = 0) {
    storage_ = storage;
    capacity_ = storage ? capacity : 0;
    size_ = writeIndex_ = 0;
    next_ = firstSequence;
    return capacity == 0 || storage != nullptr;
  }

  bool append(Record record) {
    // UINT32_MAX is the terminal exclusive cursor, never a record ID.
    if (!storage_ || capacity_ == 0 || exhausted()) return false;
    record.sequence = next_;
    record.reserved = 0;
    // The external buffer may be raw heap memory. Begin each slot's lifetime
    // explicitly, including in C++17; Record has a trivial destructor.
    ::new (static_cast<void*>(storage_ + writeIndex_)) Record(record);
    if (++writeIndex_ == capacity_) writeIndex_ = 0;
    if (size_ < capacity_) ++size_;
    ++next_;
    return true;
  }

  uint32_t oldest() const { return next_ - size_; }
  uint32_t next() const { return next_; }
  uint32_t size() const { return size_; }
  uint32_t capacity() const { return capacity_; }
  bool exhausted() const { return next_ == UINT32_MAX; }

  size_t copy(uint32_t first, Record* destination, size_t maxCount) const {
    if (!destination || maxCount == 0 || first < oldest() || first >= next_ || size_ == 0) return 0;
    size_t count = next_ - first;
    if (count > maxCount) count = maxCount;
    // Distance from the next write position avoids overflowing capacity+index.
    const uint32_t back = next_ - first;
    uint32_t index = writeIndex_ >= back ? writeIndex_ - back : capacity_ - (back - writeIndex_);
    for (size_t i = 0; i < count; ++i) {
      destination[i] = storage_[index];
      if (++index == capacity_) index = 0;
    }
    return count;
  }

private:
  Record* storage_ = nullptr;
  uint32_t capacity_ = 0, size_ = 0, writeIndex_ = 0, next_ = 0;
};
} // namespace History
