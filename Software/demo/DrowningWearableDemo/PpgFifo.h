#pragma once
#include <stdint.h>
#include "FirmwareConfig.h"

// Keep the original MAX3010x reader; detect discontinuities it cannot report.
namespace PpgFifo {
struct Poll {
  uint16_t added = 0;
  uint8_t overflow = 0;
  bool serviceGap = false;
  bool discontinuity() const { return overflow || serviceGap || added >= 4; }
};

template <class Device>
Poll poll(Device& ppg, uint32_t elapsedMs) {
  Poll result;
  // Read this BEFORE check(): popping a hardware sample clears OVF_COUNTER.
  // WR == RD can mean full after wrap, not just empty. Stock check() does not
  // distinguish those states and can return zero despite discarded samples.
  result.overflow = ppg.readRegister8(Config::PPG_ADDRESS, 0x05) & 0x1f;
  result.serviceGap = elapsedMs >= Config::PPG_SERVICE_GAP_MS;
  if (!result.overflow && !result.serviceGap) result.added = ppg.check();
  return result;
}

template <class Device>
void discard(Device& ppg) {
  while (ppg.available()) ppg.nextSample();
  ppg.clearFIFO();
}
} // namespace PpgFifo
