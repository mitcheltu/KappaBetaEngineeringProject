#pragma once
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <vector>

class TwoWire {
 public:
  std::deque<std::vector<int>> responses;
  std::deque<int> input;
  int failWrite = -1, failAck = -1, shortRead = -1, missingByte = -1;
  int writes = 0, acks = 0, requests = 0, byteReads = 0;
  uint8_t expectedAddress = 0x68;
  void beginTransmission(uint8_t address) { assert(address == expectedAddress); }
  size_t write(uint8_t) { return writes++ == failWrite ? 0 : 1; }
  size_t write(const uint8_t*, size_t length) { return writes++ == failWrite ? length - 1 : length; }
  uint8_t endTransmission(bool = true) { return acks++ == failAck ? 2 : 0; }
  size_t requestFrom(uint8_t address, size_t count, bool = true) {
    assert(address == expectedAddress);
    assert(!responses.empty());
    input = {responses.front().begin(), responses.front().end()};
    responses.pop_front();
    return requests++ == shortRead ? count - 1 : input.size();
  }
  int available() { return static_cast<int>(input.size()); }
  int read() {
    if (input.empty()) return -1;
    const int value = input.front(); input.pop_front();
    return byteReads++ == missingByte ? -1 : value;
  }
};
extern TwoWire Wire;
