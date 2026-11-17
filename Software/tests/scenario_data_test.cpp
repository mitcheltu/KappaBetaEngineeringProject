#ifdef NDEBUG
#error "These assertion tests must never be compiled with -DNDEBUG"
#endif
// Plays every compiled demo scenario (ScenarioData.h) through the real firmware
// Inference + Controller + DemoSequence with virtual time, and checks the scripted
// outcome each scenario is meant to demonstrate. Also checks the IMU CNN port against
// the LiteRT fixture. This tests the demo pipeline, not model accuracy.
#include "ScenarioController.h"
#include "ScenarioData.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace Scenario;

static uint64_t fakeUs = 0;
static uint64_t fakeClock() { return fakeUs += 50; }

struct Outcome {
  uint32_t firstGateMs = UINT32_MAX, pulseAlertMs = UINT32_MAX, oxygenAlertMs = UINT32_MAX;
  uint32_t fusedMs = UINT32_MAX, actuationMs = UINT32_MAX;
  uint32_t imuWindows = 0, gateOpenWindows = 0, pulseEvaluations = 0;
  float maxPulse = 0, maxConcern = 0;
  State finalState = State::IDLE;
};

static Outcome play(uint8_t id) {
  static CompiledSource source;
  static ConcernLevelTrack track;
  static Inference inference(track, fakeClock);
  Controller controller(source, inference, 30000);
  DemoSequence sequence;
  sequence.update(0, true, false, false, true);  // Idle, SOS released.
  Inputs in;
  in.enabled = in.batteryOk = true;
  in.nowMs = 1000;
  Command c{};
  c.opcode = static_cast<uint8_t>(Opcode::START);
  c.token = 0x4242;
  c.scenarioId = id;
  c.catalogCrc = catalogCrc(source);
  assert(controller.command(c, in) == Reason::NONE);
  Outcome o;
  uint32_t accepted = 0;
  for (uint32_t now = 1000; now < 1000 + 30000 + 115000 + 75000; now += 20) {
    in.nowMs = now;
    // Control task order: update the sequence, then offer the trigger.
    sequence.update(now, true, false, false, true);
    if (const uint32_t run = controller.takeTriggerRequest()) {
      if (sequence.requestInference(now, true, false, true, run, controller.playingRun())) accepted = run;
      else in.triggerRefusedRun = run;
    }
    in.triggerAcceptedRun = accepted;
    in.sequenceActive = sequence.stage() != DemoSequence::Stage::IDLE;
    in.sequenceTrigger = sequence.trigger();
    controller.tick(in);
    TimedEvent e;
    while (controller.popEvent(e)) {
      const uint32_t play = e.payload.elapsedMs > 30000 ? e.payload.elapsedMs - 30000 : 0;
      switch (static_cast<Event>(e.payload.event)) {
        case Event::IMU_RESULT:
          ++o.imuWindows;
          if (e.payload.flags & EV_ALERT) { ++o.gateOpenWindows; if (o.firstGateMs == UINT32_MAX) o.firstGateMs = play; }
          break;
        case Event::PULSE_RESULT:
          ++o.pulseEvaluations;
          if (std::isfinite(e.payload.value) && e.payload.value > o.maxPulse) o.maxPulse = e.payload.value;
          if ((e.payload.flags & EV_ALERT) && o.pulseAlertMs == UINT32_MAX) o.pulseAlertMs = play;
          break;
        case Event::OXYGEN_RESULT:
          if (std::isfinite(e.payload.value) && e.payload.value > o.maxConcern) o.maxConcern = e.payload.value;
          if ((e.payload.flags & EV_ALERT) && o.oxygenAlertMs == UINT32_MAX) o.oxygenAlertMs = play;
          break;
        case Event::ALERT: o.fusedMs = play; break;
        case Event::ACTUATION_START: o.actuationMs = play; break;
        default: break;
      }
    }
    if (!running(controller.state())) break;
  }
  o.finalState = controller.state();
  return o;
}

static std::string ms(uint32_t v) { return v == UINT32_MAX ? std::string("-") : std::to_string(v / 1000.0).substr(0, 5) + " s"; }

int main(int argc, char** argv) {
  const std::string root = argc > 1 ? argv[1] : "Software/tests/scenario";
  // 1) IMU CNN port vs the LiteRT interpreter (export_imu_cnn.py fixture).
  {
    std::ifstream file(root + "/imu_cnn_expected.csv");
    assert(file);
    std::string line;
    std::getline(file, line);
    static Imu::Cnn cnn;
    size_t cases = 0;
    double worst = 0;
    while (std::getline(file, line)) {
      std::stringstream s(line);
      std::string name, field;
      std::getline(s, name, ',');
      std::getline(s, field, ',');
      const double expected = std::stod(field);
      std::vector<float> w;
      while (std::getline(s, field, ',')) w.push_back(std::stof(field));
      assert(w.size() == 360);
      const double got = cnn.score(w.data());
      worst = std::max(worst, std::abs(got - expected));
      assert(std::abs(got - expected) <= 2e-6);
      assert(Imu::Cnn::gate(float(got)) == Imu::Cnn::gate(float(expected)));
      ++cases;
    }
    assert(cases >= 10);
    std::cout << "IMU CNN parity: " << cases << " windows, worst |diff|=" << worst << "\n";
    // Unit conversion: 1 g and 180 deg/s.
    const float in[6] = {1, 0, 0, 180, 0, 0};
    float out[6];
    Imu::Cnn::toModelUnits(in, out);
    assert(std::abs(out[0] - 9.80665f) < 1e-5f && std::abs(out[3] - 3.14159265f) < 1e-5f);
  }

  // 2) Catalog sanity.
  CompiledSource source;
  assert(source.count() == 4);
  for (size_t i = 0; i < source.count(); ++i) {
    Descriptor d{};
    assert(source.descriptor(i, d) && d.irSamples == 11000 && d.imuSamples == 2200 && d.spo2Samples == 110);
    assert(std::strlen(source.name(i)) > 0 && std::strlen(source.name(i)) < NAME_BYTES);
  }

  // 3) Each scenario's scripted outcome.
  for (uint8_t id = 1; id <= 4; ++id) {
    const Outcome o = play(id);
    std::cout << "scenario " << int(id) << ": IMU gate open " << o.gateOpenWindows << "/" << o.imuWindows
              << " windows (first " << ms(o.firstGateMs) << "), pulse evals " << o.pulseEvaluations
              << " max " << o.maxPulse << " alert " << ms(o.pulseAlertMs) << ", oxygen max concern "
              << o.maxConcern << " alert " << ms(o.oxygenAlertMs) << ", FUSED " << ms(o.fusedMs)
              << ", actuation " << ms(o.actuationMs) << ", final state " << int(o.finalState) << "\n";
    assert(o.imuWindows == 108 && o.pulseEvaluations > 40);
    switch (id) {
      case 1:  // Normal swim: gate opens on crawl, vitals stay normal -> nothing.
        assert(o.gateOpenWindows > 0 && o.pulseAlertMs == UINT32_MAX && o.oxygenAlertMs == UINT32_MAX);
        assert(o.fusedMs == UINT32_MAX && o.actuationMs == UINT32_MAX && o.finalState == State::DONE);
        break;
      case 2:  // Pulse stops while the gate is open -> pulse-driven actuation.
        assert(o.pulseAlertMs != UINT32_MAX && o.oxygenAlertMs == UINT32_MAX);
        assert(o.fusedMs != UINT32_MAX && o.fusedMs >= o.pulseAlertMs && o.actuationMs != UINT32_MAX);
        break;
      case 3:  // Hypoxia during a struggle -> oxygen-driven actuation.
        assert(o.oxygenAlertMs != UINT32_MAX && o.pulseAlertMs == UINT32_MAX);
        assert(o.fusedMs != UINT32_MAX && o.actuationMs != UINT32_MAX);
        break;
      case 4:  // Same hypoxia, gate closed after the oxygen alert -> no actuation.
        assert(o.oxygenAlertMs != UINT32_MAX && o.fusedMs == UINT32_MAX && o.actuationMs == UINT32_MAX);
        assert(o.finalState == State::DONE);
        break;
    }
  }
  std::cout << "Scenario data playback, IMU gate fusion and IMU CNN parity tests passed\n";
}
