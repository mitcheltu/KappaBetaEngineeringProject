#pragma once
#include <stdint.h>
#include "FirmwareConfig.h"

// Readback through the original MAX3010x library; acquisition still uses check().
namespace PpgDiagnostics {
struct Configuration {
  uint8_t part, revision, mode, spo2, fifo, redLed, irLed;

  bool matches() const {
    static_assert(Config::PPG_SAMPLE_HZ == 100 && Config::PPG_AVERAGING == 4,
                  "Update the readback expectations when changing PPG timing.");
    // Red+IR, awake, reset finished; 100 Hz/411 us/4096 nA; average four,
    // rollover enabled. FIFO almost-full threshold is not used by polling.
    return part == 0x15 && mode == 0x03 && (spo2 & 0x7f) == 0x27 &&
           (fifo & 0xf0) == 0x50 && redLed == Config::PPG_LED_AMPLITUDE &&
           irLed == Config::PPG_LED_AMPLITUDE;
  }
};

template <class Device>
Configuration readConfiguration(Device& ppg) {
  return {ppg.readRegister8(Config::PPG_ADDRESS, 0xff),
          ppg.readRegister8(Config::PPG_ADDRESS, 0xfe),
          ppg.readRegister8(Config::PPG_ADDRESS, 0x09),
          ppg.readRegister8(Config::PPG_ADDRESS, 0x0a),
          ppg.readRegister8(Config::PPG_ADDRESS, 0x08),
          ppg.readRegister8(Config::PPG_ADDRESS, 0x0c),
          ppg.readRegister8(Config::PPG_ADDRESS, 0x0d)};
}
} // namespace PpgDiagnostics
