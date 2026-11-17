#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>

class TwoWire {
 public:
  uint8_t registers[256] = {};
  std::deque<uint8_t> fifo, response;
  uint8_t selected = 0;
  unsigned writeCount = 0, fifoByte = 0;
  int ignoredWriteRegister = -1;
  bool failReads = false;
  unsigned fifoReadRequests = 0;
  void begin() {}
  void setClock(uint32_t) {}
  void beginTransmission(uint8_t address) { assert(address == 0x57); writeCount = 0; }
  size_t write(uint8_t value) {
    if (writeCount++ == 0) selected = value;
    else {
      if (selected != ignoredWriteRegister) registers[selected] = value;
      if (selected == 0x09) registers[selected] &= ~0x40; // Reset completes immediately.
      if (selected == 0x06) fifoByte = 0;
      ++selected;
    }
    return 1;
  }
  uint8_t endTransmission(bool = true) { return 0; }
  size_t requestFrom(uint8_t address, int count) {
    assert(address == 0x57);
    response.clear();
    if (failReads) return 0;
    if (selected == 0x07) {
      ++fifoReadRequests;
      for (int i = 0; i < count; ++i) {
        assert(!fifo.empty());
        response.push_back(fifo.front()); fifo.pop_front();
        if (++fifoByte == 6) {
          fifoByte = 0;
          registers[0x06] = (registers[0x06] + 1) & 31;
          registers[0x05] = 0; // A completed hardware FIFO pop clears overflow.
        }
      }
    } else {
      for (int i = 0; i < count; ++i) response.push_back(registers[selected++]);
    }
    return count;
  }
  int available() { return static_cast<int>(response.size()); }
  int read() {
    if (response.empty()) return -1;
    int result = response.front(); response.pop_front(); return result;
  }
  void enqueuePair(uint32_t red, uint32_t ir) {
    for (uint32_t value : {red, ir}) {
      fifo.push_back(static_cast<uint8_t>(value >> 16));
      fifo.push_back(static_cast<uint8_t>(value >> 8));
      fifo.push_back(static_cast<uint8_t>(value));
    }
    registers[0x04] = (registers[0x04] + 1) & 31;
  }
};
extern TwoWire Wire;
