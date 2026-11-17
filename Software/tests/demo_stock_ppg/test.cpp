// Links the unchanged SparkFun MAX3010x 1.1.2 MAX30105.cpp against fake I2C.
// Protects the demo's public-API FIFO consumption order and overflow handling.
// See README.md for the host compiler invocation; do not build with -DNDEBUG.
#include <MAX30105.h>
#include <cassert>
#include <cstdio>
#include "PpgDiagnostics.h"
#include "PpgFifo.h"

TwoWire Wire;
uint32_t testMillis = 0;
MAX30105 ppg; // Global, as in the Arduino sketch.

static void readExpected(uint32_t red, uint32_t ir) {
  assert(ppg.available());
  ppg.nextSample();
  assert(ppg.getFIFORed() == red);
  assert(ppg.getFIFOIR() == ir);
}

int main() {
  Wire.registers[0xff] = 0x15;
  assert(ppg.begin(Wire, 400000, 0x57));
  ppg.setup(60, 4, 2, 100, 411, 4096);
  ppg.enableFIFORollover();
  assert(PpgDiagnostics::readConfiguration(ppg).matches());
  Wire.registers[0x08] |= 0x0f; // The user's module retains an almost-full threshold of 15.
  assert(PpgDiagnostics::readConfiguration(ppg).matches());
  Wire.enqueuePair(51001, 61001);
  assert(PpgDiagnostics::readConfiguration(ppg).matches());
  assert(Wire.fifo.size() == 6); // Diagnostic reads must not consume optical data.
  assert(ppg.check() == 1);
  readExpected(51001, 61001);
  assert(ppg.available() == 0);
  for (unsigned i = 2; i <= 4; ++i) Wire.enqueuePair(51000 + i, 61000 + i);
  assert(ppg.check() == 3);
  for (unsigned i = 2; i <= 4; ++i) readExpected(51000 + i, 61000 + i);
  assert(ppg.available() == 0);
  for (unsigned i = 5; i <= 8; ++i) Wire.enqueuePair(51000 + i, 61000 + i);
  assert(ppg.check() == STORAGE_SIZE);
  assert(ppg.available() == 0); // A full four-pair batch aliases empty: must reject.
  ppg.clearFIFO();
  Wire.enqueuePair(51009, 61009);
  assert(ppg.check() == 1);
  readExpected(51009, 61009);
  for (unsigned i = 10; i <= 14; ++i) Wire.enqueuePair(51000 + i, 61000 + i);
  assert(ppg.check() == 5); // Overwrite must also be rejected, not interpreted as one pair.
  assert(ppg.available() == 1);
  while (ppg.available()) ppg.nextSample();
  ppg.clearFIFO();
  Wire.enqueuePair(51015, 61015);
  assert(ppg.check() == 1);
  readExpected(51015, 61015);

  // Reproduce the observed device state: equal pointers with lost samples.
  // Stock check() incorrectly looks empty; our actual polling helper must
  // report discontinuity and recover before publishing/estimating any samples.
  PpgFifo::discard(ppg);
  Wire.registers[0x05] = 25;
  assert(ppg.getWritePointer() == 0 && ppg.getReadPointer() == 0);
  assert(ppg.check() == 0);
  auto lost = PpgFifo::poll(ppg, 5);
  assert(lost.discontinuity() && lost.overflow == 25 && lost.added == 0);
  PpgFifo::discard(ppg);
  assert(Wire.registers[0x05] == 0);
  Wire.enqueuePair(51016, 61016);
  auto recovered = PpgFifo::poll(ppg, 5);
  assert(!recovered.discontinuity() && recovered.added == 1);
  readExpected(51016, 61016);

  // Check overflow before data reads, since popping would erase the evidence.
  Wire.enqueuePair(51017, 61017);
  Wire.registers[0x05] = 31;
  const auto readsBefore = Wire.fifoReadRequests;
  assert(PpgFifo::poll(ppg, 5).overflow == 31);
  assert(Wire.fifoReadRequests == readsBefore && Wire.registers[0x05] == 31);
  // Also do not copy an overdue hardware batch into the tiny software ring.
  Wire.registers[0x05] = 0;
  assert(PpgFifo::poll(ppg, Config::PPG_SERVICE_GAP_MS).serviceGap);
  assert(Wire.fifoReadRequests == readsBefore);
  assert(PpgFifo::poll(ppg, Config::PPG_SERVICE_GAP_MS - 1).added == 1);
  readExpected(51017, 61017);
  for (unsigned i = 18; i <= 21; ++i) Wire.enqueuePair(51000 + i, 61000 + i);
  const auto wrapped = PpgFifo::poll(ppg, 5);
  assert(wrapped.added == STORAGE_SIZE && wrapped.discontinuity());
  PpgFifo::discard(ppg);

  // A successful identity read does not prove that setup's unchecked writes
  // worked. Reject a lost mode write even though stock begin() still succeeds.
  Wire = TwoWire{};
  Wire.registers[0xff] = 0x15;
  Wire.ignoredWriteRegister = 0x09;
  assert(ppg.begin(Wire, 400000, 0x57));
  ppg.setup(Config::PPG_LED_AMPLITUDE, Config::PPG_AVERAGING, 2, Config::PPG_SAMPLE_HZ, 411, 4096);
  ppg.enableFIFORollover();
  assert(!PpgDiagnostics::readConfiguration(ppg).matches());
  Wire.ignoredWriteRegister = -1;
  ppg.setup(Config::PPG_LED_AMPLITUDE, Config::PPG_AVERAGING, 2, Config::PPG_SAMPLE_HZ, 411, 4096);
  ppg.enableFIFORollover();
  assert(PpgDiagnostics::readConfiguration(ppg).matches());
  // Shutdown/reset, rate drift, averaging/rollover changes and failed LED
  // writes all invalidate readiness, rather than masquerading as low contact.
  const uint8_t faults[][2] = {{0x09, 0x83}, {0x09, 0x43}, {0x0a, 0x23},
                              {0x08, 0x00}, {0x08, 0x40}, {0x0c, 0}, {0x0d, 0}, {0xff, 0}};
  for (const auto& fault : faults) {
    const auto original = Wire.registers[fault[0]];
    Wire.registers[fault[0]] = fault[1];
    assert(!PpgDiagnostics::readConfiguration(ppg).matches());
    Wire.registers[fault[0]] = original;
  }
  Wire.failReads = true;
  assert(!PpgDiagnostics::readConfiguration(ppg).matches());
  std::puts("PASS: stock MAX3010x FIFO pairing, full/empty alias and overflow recovery, setup readback and injected faults.");
}
