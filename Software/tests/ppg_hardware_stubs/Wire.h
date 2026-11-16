#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

class TwoWire {
 public:
  std::array<std::uint8_t, 256> registers{};
  std::deque<std::array<std::uint8_t, 6>> fifo;
  std::vector<std::uint8_t> tx, rx;
  std::size_t readIndex = 0;
  std::uint8_t selected = 0;
  int failReadReg = -1;
  int shortReadReg = -1;
  bool beginOk = true, writeOk = true;
  std::uint8_t endStatus = 0;
  unsigned timeoutMs = 0;
  bool begin(int, int, std::uint32_t) { return beginOk; }
  void setTimeOut(unsigned ms) { timeoutMs = ms; }
  void beginTransmission(std::uint8_t) { tx.clear(); }
  std::size_t write(std::uint8_t value) {
    if (!writeOk) return 0;
    tx.push_back(value); return 1;
  }
  std::uint8_t endTransmission(bool = true) {
    if (endStatus) return endStatus;
    if (tx.empty()) return 4;
    selected = tx[0];
    for (std::size_t i = 1; i < tx.size(); ++i)
      registers[static_cast<std::uint8_t>(selected + i - 1)] = tx[i];
    return 0;
  }
  std::size_t requestFrom(std::uint8_t, std::size_t count, bool) {
    rx.clear(); readIndex = 0;
    if (selected == failReadReg) return 0;
    const auto actual = selected == shortReadReg && count ? count - 1 : count;
    if (selected == 0x07 && count == 6 && !fifo.empty()) {
      const auto sample = fifo.front(); fifo.pop_front();
      rx.assign(sample.begin(), sample.begin() + actual);
      registers[0x06] = (registers[0x06] + 1) & 0x1f;
    } else {
      for (std::size_t i = 0; i < actual; ++i)
        rx.push_back(registers[static_cast<std::uint8_t>(selected + i)]);
    }
    return actual;
  }
  int available() const { return static_cast<int>(rx.size() - readIndex); }
  int read() { return readIndex < rx.size() ? rx[readIndex++] : -1; }
};
extern TwoWire Wire;
