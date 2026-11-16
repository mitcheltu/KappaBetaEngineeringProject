#pragma once
#include "Wire.h"
class MAX30105 {
 public:
  bool beginOk = true;
  bool badAveragingReadback = false;
  unsigned average = 0, mode = 0, rate = 0, width = 0, range = 0;
  bool begin(TwoWire&, std::uint32_t, std::uint8_t) { return beginOk; }
  void setup(std::uint8_t current, unsigned averaging, unsigned ledMode,
             unsigned sampleRate, unsigned pulseWidth, unsigned adcRange) {
    average = averaging; mode = ledMode; rate = sampleRate;
    width = pulseWidth; range = adcRange;
    Wire.registers[0x09] = 3;
    Wire.registers[0x0a] = 0x27;
    Wire.registers[0x08] = badAveragingReadback ? 0x40 : 0;
    Wire.registers[0x0c] = Wire.registers[0x0d] = current;
  }
  void disableFIFORollover() {}
};
