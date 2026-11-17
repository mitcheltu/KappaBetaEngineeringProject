#pragma once
// HOST-TEST DOUBLE ONLY. Not a demo scenario, not physiological data, not a
// label. It produces simple arithmetic signals (a sine "pulse" that can stop,
// and a linear SpO2 ramp) so the real controller/inference code can be driven
// with virtual time. Demo scenario data are intentionally not created yet.
#include <math.h>
#include "ScenarioSource.h"

struct FakeScenarioSourceTestDouble final : Scenario::Source {
  struct Script {
    uint8_t id = 7;
    uint32_t irSamples = 4000;        // 40 s at 100 Hz.
    uint32_t pulseStopsAtSample = UINT32_MAX;  // Flat (constant) IR afterwards.
    uint32_t nanAtSample = UINT32_MAX;         // One non-finite IR sample.
    uint32_t spo2Seconds = 40;
    float spo2Start = 97.0f, spo2SlopePerS = 0.0f, spo2Floor = 50.0f;
    uint32_t spo2DeclineFrom = 0, spo2InvalidFrom = UINT32_MAX;
  };
  Script script;
  bool selected = false;

  size_t count() const override { return 1; }
  bool descriptor(size_t index, Scenario::Descriptor& out) const override {
    if (index != 0) return false;
    out = Scenario::Descriptor{};
    out.id = script.id;
    out.version = 1;
    out.irSamples = script.irSamples;
    out.spo2Samples = script.spo2Seconds;
    out.contentCrc32 = 0x12345678u;  // Arbitrary test value.
    return true;
  }
  bool select(uint8_t id) override { selected = id == script.id; return selected; }
  uint32_t irCount() const override { return selected ? script.irSamples : 0; }
  uint32_t spo2Count() const override { return selected ? script.spo2Seconds : 0; }
  bool ir(uint32_t i, float& value) const override {
    if (!selected || i >= script.irSamples) return false;
    if (i == script.nanAtSample) { value = NAN; return true; }
    if (i >= script.pulseStopsAtSample) { value = 1000.0f; return true; }
    // 1.2 Hz sine with a sharper systolic peak (squared positive half).
    const double phase = 2.0 * 3.14159265358979323846 * 1.2 * (i / 100.0);
    const double s = sin(phase);
    value = static_cast<float>(1000.0 + 200.0 * (s > 0 ? s * s : 0.2 * s));
    return true;
  }
  bool spo2(uint32_t second, float& value, bool& valid) const override {
    if (!selected || second >= script.spo2Seconds) return false;
    valid = second < script.spo2InvalidFrom;
    float v = script.spo2Start;
    if (second >= script.spo2DeclineFrom)
      v -= script.spo2SlopePerS * float(second - script.spo2DeclineFrom);
    value = v < script.spo2Floor ? script.spo2Floor : v;
    return true;
  }
};
