#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

class TwoWire {
public:
  std::deque<std::vector<int>> responses;
  std::deque<int> incoming;
  std::vector<uint8_t> commands;
  int failWrite = -1, failEnd = -1, shortRequest = -1, shortAvailable = -1;
  int writeCalls = 0, endCalls = 0, requestCalls = 0, availableCalls = 0;
  void beginTransmission(uint8_t address) { assert(address == 0x76); }
  size_t write(uint8_t value) {
    commands.push_back(value);
    return writeCalls++ == failWrite ? 0 : 1;
  }
  uint8_t endTransmission() { return endCalls++ == failEnd ? 2 : 0; }
  size_t requestFrom(uint8_t address, uint8_t length) {
    assert(address == 0x76);
    assert(!responses.empty());
    incoming = std::deque<int>(responses.front().begin(), responses.front().end());
    responses.pop_front();
    return requestCalls++ == shortRequest ? length - 1 : incoming.size();
  }
  int available() { return availableCalls++ == shortAvailable ? 0 : static_cast<int>(incoming.size()); }
  int read() {
    if (incoming.empty()) return -1;
    const int value = incoming.front();
    incoming.pop_front();
    return value;
  }
};
extern TwoWire Wire;
