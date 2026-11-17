// Drives the real DemoInference (PulseLoss + PpgWindow + OxygenConcern copies in
// the demo sketch) with the host test double and virtual time. Expected values
// come from calling the same model code directly on the same windows, so the
// test checks wiring/cadence/latching, not detector effectiveness.
#include "DemoInference.h"
#include "scenario_test_double.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

using namespace Scenario;

static uint64_t fakeUs = 0;
static uint64_t fakeClock() { return fakeUs += 1234; }  // Each read advances 1.234 ms.

static Ppg::PulseLoss reference;

struct Collected {
  std::vector<PulseOutcome> pulses;
  std::vector<OxygenOutcome> oxygen;
};

static Collected run(Inference& inference, FakeScenarioSourceTestDouble& source,
                     uint32_t endMs, uint32_t stepMs) {
  Collected c;
  assert(source.select(source.script.id));
  inference.begin(&source);
  for (uint32_t t = 0;; t += stepMs) {
    const uint32_t now = t > endMs ? endMs : t;
    for (unsigned guard = 0; guard < 100000; ++guard) {
      const Step s = inference.advance(now);
      if (!s.pulse && !s.oxygen) break;
      if (s.pulse) c.pulses.push_back(s.pulseOutcome);
      if (s.oxygen) c.oxygen.push_back(s.oxygenOutcome);
    }
    if (now == endMs) break;
  }
  return c;
}

static void expectPulseMatchesDirect(const FakeScenarioSourceTestDouble& source,
                                     const PulseOutcome& p, Ppg::PulseModel model) {
  static float window[Ppg::PpgWindow::kSamples];
  const uint32_t first = p.windowEndSample - Ppg::PpgWindow::kSamples;
  for (uint32_t i = 0; i < Ppg::PpgWindow::kSamples; ++i) {
    float v = 0;
    assert(source.ir(first + i, v));
    window[i] = v;
  }
  const auto r = reference.evaluate(window, model, Ppg::PulseOperatingPoint::HighSpecificity);
  assert(r.valid == p.valid);
  assert(r.alert == p.alert);
  if (r.valid) assert(static_cast<float>(r.score) == p.score);
}

int main() {
  // 1) Cadence + equivalence for every model, steady pulse then flatline.
  for (Ppg::PulseModel model : {Ppg::PulseModel::Rule, Ppg::PulseModel::Logistic,
                                Ppg::PulseModel::BoostedTrees, Ppg::PulseModel::Cnn}) {
    ConcernLevelTrack track;
    InferenceConfig config;
    config.model = model;
    config.fusion = Fusion::VITALS_ONLY;
    // ~80 KB workspace: heap on the host, static storage on the device.
    auto inference = std::make_unique<Inference>(track, fakeClock, config);
    FakeScenarioSourceTestDouble source;
    source.script.irSamples = 4000;
    source.script.pulseStopsAtSample = 2400;  // Pulse stops at 24 s.
    const Collected c = run(*inference, source, 40000, 5);
    // First evaluation when the 16-s ring first fills (sample 1600, t=15.99 s),
    // then every 200 samples: 1 + (4000-1600)/200 = 13 evaluations.
    assert(c.pulses.size() == 13);
    for (size_t i = 0; i < c.pulses.size(); ++i) {
      assert(c.pulses[i].windowEndSample == 1600 + 200 * i);
      assert(c.pulses[i].elapsedMs == (1599 + 200 * i) * 10);
      assert(c.pulses[i].analysisUs == 1234);  // One fake-clock step per evaluation.
      expectPulseMatchesDirect(source, c.pulses[i], model);
    }
    bool anyAlert = false;
    for (const auto& p : c.pulses) anyAlert = anyAlert || p.alert;
    assert(inference->pulseActive() == anyAlert);  // Latched once any window alerts.
    assert(inference->evaluations() == 13 && inference->maxAnalysisUs() == 1234);
    assert(inference->finished());
    if (model == Ppg::PulseModel::Rule) {
      // Rule definition: no pulse >= 4 s. Windows ending >= 4 s after the stop
      // must alert; windows entirely before the stop with a steady sine must not.
      for (const auto& p : c.pulses) {
        if (p.windowEndSample >= 2400 + 400) assert(p.alert);
        if (p.windowEndSample <= 2400) assert(!p.alert);
      }
      assert(anyAlert);
    }
    std::cout << "model " << int(model) << ": 13 evaluations match direct PulseLoss; latched="
              << anyAlert << "\n";
  }

  // 2) Catch-up: one huge advance reproduces the fine-grained results exactly.
  {
    static ConcernLevelTrack trackA, trackB;
    InferenceConfig config;
    config.model = Ppg::PulseModel::Logistic;
    config.fusion = Fusion::VITALS_ONLY;
    static Inference fine(trackA, fakeClock, config), coarse(trackB, fakeClock, config);
    FakeScenarioSourceTestDouble a, b;
    a.script.spo2SlopePerS = b.script.spo2SlopePerS = 1.5f;
    a.script.spo2DeclineFrom = b.script.spo2DeclineFrom = 10;
    const Collected cf = run(fine, a, 40000, 5), cc = run(coarse, b, 40000, 40000);
    assert(cf.pulses.size() == cc.pulses.size() && cf.oxygen.size() == cc.oxygen.size());
    for (size_t i = 0; i < cf.pulses.size(); ++i)
      assert(cf.pulses[i].score == cc.pulses[i].score && cf.pulses[i].alert == cc.pulses[i].alert);
    for (size_t i = 0; i < cf.oxygen.size(); ++i)
      assert((std::isnan(cf.oxygen[i].concern) && std::isnan(cc.oxygen[i].concern)) ||
             cf.oxygen[i].concern == cc.oxygen[i].concern);
  }

  // 3) Oxygen track equals a directly driven OxygenConcern on the same readings.
  {
    static ConcernLevelTrack track;
    static Inference inference(track, fakeClock);
    FakeScenarioSourceTestDouble source;
    source.script.irSamples = 0;
    source.script.spo2Seconds = 60;
    source.script.spo2DeclineFrom = 5;
    source.script.spo2SlopePerS = 1.0f;   // 97 -> 50 floor.
    source.script.spo2InvalidFrom = 55;   // Estimator unavailable at the end.
    const Collected c = run(inference, source, 60000, 5);
    assert(c.pulses.empty() && c.oxygen.size() == 60);
    Ppg::OxygenConcern direct;
    bool everActive = false;
    for (uint32_t s = 0; s < 60; ++s) {
      float v = 0;
      bool valid = false;
      assert(source.spo2(s, v, valid));
      direct.update(v, valid, s);
      const OxygenOutcome& o = c.oxygen[s];
      assert(o.second == s && o.elapsedMs == s * 1000);
      assert(o.active == direct.active());
      assert((std::isnan(o.concern) && std::isnan(direct.concern())) || o.concern == direct.concern());
      everActive = everActive || o.active;
    }
    assert(everActive && inference.oxygenActive());  // Latch survives invalid readings.
    assert(inference.vitalsAlert() && !inference.pulseActive());
    // Default fusion needs the IMU gate open; this source has no IMU rows.
    assert(!inference.imuGateOpen() && !inference.alert());
  }

  // 4) A non-finite scripted IR sample resets the ring: no evaluation until 16 s
  //    of new contiguous samples, never a window bridging the gap.
  {
    static ConcernLevelTrack track;
    InferenceConfig config;
    config.model = Ppg::PulseModel::Rule;
    config.fusion = Fusion::VITALS_ONLY;
    static Inference inference(track, fakeClock, config);
    FakeScenarioSourceTestDouble source;
    source.script.irSamples = 4000;
    source.script.nanAtSample = 2000;
    const Collected c = run(inference, source, 40000, 5);
    for (const auto& p : c.pulses) assert(p.windowEndSample <= 2000 || p.windowEndSample >= 2001 + 1600);
    assert(c.pulses.front().windowEndSample == 1600);
    bool resumed = false;
    for (const auto& p : c.pulses) resumed = resumed || p.windowEndSample == 3601;
    assert(resumed);
  }

  // 5) No source / stopped: nothing is consumed.
  {
    static ConcernLevelTrack track;
    static Inference inference(track, fakeClock);
    const Step s = inference.advance(100000);
    assert(!s.pulse && !s.oxygen && inference.finished());
  }
  std::cout << "Scenario inference cadence, model equivalence, catch-up, oxygen latch "
               "and gap tests passed\n";
}
