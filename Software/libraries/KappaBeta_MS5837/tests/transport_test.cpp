#include "KappaBetaMS5837.h"
#include <cassert>
#include <cmath>
#include <iostream>

TwoWire Wire;

// Manufacturer example C1-C6; CRC4=2 for C0 lower bits=0.
void prom(TwoWire& bus) {
  for (uint16_t value : {0x2000, 34982, 36352, 20328, 22354, 26646, 26146})
    bus.responses.push_back({value >> 8, value & 255});
}
void adc(TwoWire& bus, uint32_t d1 = 4958179, uint32_t d2 = 6815414) {
  for (uint32_t value : {d1, d2})
    bus.responses.push_back({static_cast<int>(value >> 16), static_cast<int>((value >> 8) & 255), static_cast<int>(value & 255)});
}
void invalid(KappaBetaMS5837& sensor) {
  assert(std::isnan(sensor.pressure()));
  assert(std::isnan(sensor.temperature()));
  assert(std::isnan(sensor.depth()));
  assert(std::isnan(sensor.altitude()));
}
void initialize(KappaBetaMS5837& sensor, TwoWire& bus) {
  prom(bus);
  assert(sensor.init(bus));
  sensor.setModel(KappaBetaMS5837::MS5837_30BA);
  invalid(sensor);
}
int main() {
  {
    KappaBetaMS5837 sensor;
    invalid(sensor);
    assert(!sensor.read());
    TwoWire bus;
    initialize(sensor, bus);
    adc(bus);
    assert(sensor.read());
    // Upstream uses integer divisions (19.82 rather than rounded 19.81 C).
    assert(std::fabs(sensor.pressure() - 3999.8f) < 0.2f);
    assert(std::fabs(sensor.pressure(KappaBetaMS5837::Pa) - 399980.0f) < 20.0f);
    assert(std::fabs(sensor.temperature() - 19.82f) < 0.011f);
    assert(bus.commands == std::vector<uint8_t>({0x1e, 0xa0, 0xa2, 0xa4, 0xa6, 0xa8, 0xaa, 0xac, 0x4a, 0x00, 0x5a, 0x00}));
  }
  // Failure at any command phase must reject a conversion and invalidate old data.
  for (int phase = 0; phase < 4; ++phase) {
    for (bool nack : {false, true}) {
      TwoWire bus;
      KappaBetaMS5837 sensor;
      initialize(sensor, bus);
      adc(bus);
      assert(sensor.read());
      adc(bus);
      if (nack) bus.failEnd = bus.endCalls + phase;
      else bus.failWrite = bus.writeCalls + phase;
      assert(!sensor.read());
      invalid(sensor);
    }
  }
  for (int phase = 0; phase < 2; ++phase) {
    TwoWire bus;
    KappaBetaMS5837 sensor;
    initialize(sensor, bus);
    adc(bus);
    assert(sensor.read());
    bus.shortRequest = bus.requestCalls + phase;
    adc(bus);
    assert(!sensor.read());
    invalid(sensor);
    // Recovery needs a new successful complete conversion, never a stale value.
    bus.responses.clear();
    adc(bus);
    assert(sensor.read());
    assert(std::isfinite(sensor.pressure()));
  }
  for (int byteIndex = 0; byteIndex < 6; ++byteIndex) {
    TwoWire bus;
    KappaBetaMS5837 sensor;
    initialize(sensor, bus);
    adc(bus);
    bus.responses[byteIndex / 3][byteIndex % 3] = -1;
    assert(!sensor.read());
    invalid(sensor);
  }
  for (int byteIndex = 0; byteIndex < 6; ++byteIndex) {
    TwoWire bus;
    KappaBetaMS5837 sensor;
    initialize(sensor, bus);
    adc(bus);
    bus.shortAvailable = bus.availableCalls + byteIndex;
    assert(!sensor.read());
    invalid(sensor);
  }
  {
    TwoWire bus;
    KappaBetaMS5837 sensor;
    initialize(sensor, bus);
    adc(bus, 4958179, 8000000);
    assert(sensor.read());
    // Datasheet high-temperature formula requires a 64-bit dT squared term.
    assert(std::fabs(sensor.temperature() - 56.53f) < 0.011f);
  }
  for (uint32_t bad : {0u, 0xffffffu}) {
    for (bool first : {false, true}) {
      TwoWire bus;
      KappaBetaMS5837 sensor;
      initialize(sensor, bus);
      adc(bus, first ? bad : 4958179, first ? 6815414 : bad);
      assert(!sensor.read());
      invalid(sensor);
    }
  }
  for (int phase = 0; phase < 8; ++phase) {
    TwoWire bus;
    KappaBetaMS5837 sensor;
    prom(bus);
    bus.failEnd = phase;
    assert(!sensor.init(bus));
    assert(!sensor.read());
    invalid(sensor);
  }
  for (int phase = 0; phase < 7; ++phase) {
    TwoWire bus;
    KappaBetaMS5837 sensor;
    prom(bus);
    bus.shortRequest = phase;
    assert(!sensor.init(bus));
    assert(!sensor.read());
  }
  {
    TwoWire bus;
    KappaBetaMS5837 sensor;
    prom(bus);
    bus.responses[2][1] ^= 1;
    assert(!sensor.init(bus)); // Single-bit PROM corruption must fail CRC.
    invalid(sensor);
  }
  for (int value : {0, 255}) {
    TwoWire bus;
    KappaBetaMS5837 sensor;
    for (int i = 0; i < 7; ++i) bus.responses.push_back({value, value});
    assert(!sensor.init(bus));
  }
  {
    TwoWire bus;
    KappaBetaMS5837 sensor;
    initialize(sensor, bus);
    adc(bus);
    assert(sensor.read());
    prom(bus);
    bus.failEnd = bus.endCalls;
    assert(!sensor.init(bus));
    invalid(sensor);
    assert(!sensor.read());
  }
  std::cout << "MS5837 transport, CRC, compensation, recovery and invalidation tests passed\n";
}
