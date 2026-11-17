// Scenario command parsing, START/ABORT rejection rules, the 30 s ARMED buffer,
// Cancel priority, and the full ALERT -> DemoSequence(INFERENCE) -> DONE path,
// using the real controller/inference/DemoSequence with virtual time. The
// control-task emulation below mirrors Actuators.ino's ordering.
#include "ScenarioController.h"
#include "scenario_test_double.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

using namespace Scenario;

static uint64_t fakeUs = 0;
static uint64_t fakeClock() { return fakeUs += 100; }

static Command start(uint16_t token, uint8_t id, uint16_t crc) {
  Command c{};
  c.opcode = static_cast<uint8_t>(Opcode::START);
  c.token = token; c.scenarioId = id; c.catalogCrc = crc;
  return c;
}
static Command abortCommand(uint16_t token) {
  Command c{};
  c.opcode = static_cast<uint8_t>(Opcode::ABORT);
  c.token = token;
  return c;
}

struct Rig {
  FakeScenarioSourceTestDouble source;
  ConcernLevelTrack track;
  std::unique_ptr<Inference> inference;
  std::unique_ptr<Controller> controller;
  DemoSequence sequence;
  Inputs in;
  uint32_t requestRun = 0;
  std::vector<TimedEvent> events;
  bool cancel = false, batteryOk = true, sos = false;

  explicit Rig(Ppg::PulseModel model = Ppg::PulseModel::Rule) {
    InferenceConfig config;
    config.model = model;
    // This rig's test double has no IMU rows; the IMU gate is covered by scenario_data_test.
    config.fusion = Fusion::VITALS_ONLY;
    inference = std::make_unique<Inference>(track, fakeClock, config);
    controller = std::make_unique<Controller>(source, *inference, 30000);
    in.enabled = true;
    in.batteryOk = true;
    sequence.update(0, true, false, false, true);  // Idle, SOS released.
  }
  uint16_t crc() const { return catalogCrc(source); }
  void inputs(uint32_t now) {
    in.nowMs = now; in.cancel = cancel; in.batteryOk = batteryOk; in.sosHeld = sos;
    in.sequenceActive = sequence.stage() != DemoSequence::Stage::IDLE;
    in.sequenceTrigger = sequence.trigger();
  }
  // Control task tick (5 ms): update, then offer the one-shot request.
  void control(uint32_t now) {
    sequence.update(now, true, cancel, sos, batteryOk);
    if (requestRun) {
      const bool ok = sequence.requestInference(now, true, cancel, batteryOk, requestRun,
                                                controller->playingRun());
      (ok ? in.triggerAcceptedRun : in.triggerRefusedRun) = requestRun;
      requestRun = 0;
    }
  }
  void step(uint32_t now) {
    control(now);
    inputs(now);
    controller->tick(in);
    if (const uint32_t r = controller->takeTriggerRequest()) requestRun = r;
    TimedEvent e;
    while (controller->popEvent(e)) events.push_back(e);
  }
  Reason send(const Command& c, uint32_t now) {
    inputs(now);
    const Reason r = controller->command(c, in);
    TimedEvent e;
    while (controller->popEvent(e)) events.push_back(e);
    return r;
  }
  size_t count(Event type) const {
    size_t n = 0;
    for (const auto& e : events) n += e.payload.event == static_cast<uint8_t>(type);
    return n;
  }
  const TimedEvent* first(Event type) const {
    for (const auto& e : events) if (e.payload.event == static_cast<uint8_t>(type)) return &e;
    return nullptr;
  }
};

int main() {
  // ---- Wire parsing ----
  {
    const uint8_t ok[8] = {1, 1, 0x34, 0x12, 7, 0, 0xcd, 0xab};
    Command c{};
    assert(parseCommand(ok, 8, c) && c.opcode == 1 && c.token == 0x1234 &&
           c.scenarioId == 7 && c.catalogCrc == 0xabcd);
    uint8_t bad[8];
    assert(!parseCommand(ok, 7, c) && !parseCommand(nullptr, 8, c));
    memcpy(bad, ok, 8); bad[1] = 2; assert(!parseCommand(bad, 8, c));            // version
    memcpy(bad, ok, 8); bad[2] = bad[3] = 0; assert(!parseCommand(bad, 8, c));   // token 0
    memcpy(bad, ok, 8); bad[5] = 1; assert(!parseCommand(bad, 8, c));            // reserved
    for (uint8_t op : {0, 3, 255}) { memcpy(bad, ok, 8); bad[0] = op; assert(!parseCommand(bad, 8, c)); }
    memcpy(bad, ok, 8); bad[0] = 2; assert(parseCommand(bad, 8, c));             // ABORT
    assert(scaleUnit(NAN) == UINT16_MAX && scaleUnit(-1) == 0 && scaleUnit(2) == 10000 &&
           scaleUnit(0.5f) == 5000);
  }

  // ---- Null source: empty catalog, every START is UNKNOWN_SCENARIO ----
  {
    NullSource null;
    ConcernLevelTrack track;
    auto inference = std::make_unique<Inference>(track, fakeClock);
    Controller controller(null, *inference);
    assert(catalogCrc(null) == 0x0000);
    Inputs in; in.enabled = true; in.batteryOk = true;
    for (uint8_t id : {0, 1, 7, 255})
      assert(controller.command(start(5, id, 0), in) == Reason::UNKNOWN_SCENARIO);
    assert(controller.state() == State::REJECTED && controller.playingRun() == 0);
    const Status s = controller.status(0);
    assert(s.state == 7 && s.reason == 4 && s.commandToken == 5 && s.catalogCrc == 0 &&
           s.pulseScore == UINT16_MAX && s.oxygenConcern == UINT16_MAX && s.remainingMs == 0);
  }

  // ---- START rejection rules (in priority order) ----
  {
    Rig rig;
    const uint16_t crc = rig.crc();
    rig.in.enabled = false;  assert(rig.send(start(1, 7, crc), 0) == Reason::SCENARIOS_DISABLED);
    rig.in.enabled = true;
    rig.cancel = true;       assert(rig.send(start(2, 7, crc), 0) == Reason::CANCEL_HELD);
    rig.cancel = false;
    rig.batteryOk = false;   assert(rig.send(start(3, 7, crc), 0) == Reason::BATTERY_NOT_READY);
    rig.batteryOk = true;
    rig.sos = true;          assert(rig.send(start(4, 7, crc), 0) == Reason::SOS_HELD);
    rig.sos = false;
    assert(rig.send(start(5, 8, crc), 0) == Reason::UNKNOWN_SCENARIO);
    assert(rig.send(start(6, 7, uint16_t(crc ^ 1)), 0) == Reason::CATALOG_MISMATCH);
    assert(rig.controller->state() == State::REJECTED && rig.count(Event::REJECTED) == 6);
    assert(rig.events.back().payload.stateOrReason == uint8_t(Reason::CATALOG_MISMATCH) &&
           rig.events.back().payload.runToken == 6);
    // A running SOS sequence blocks START.
    rig.sequence.update(1, true, false, true, true);
    assert(rig.sequence.stage() == DemoSequence::Stage::VIBRATING);
    assert(rig.send(start(7, 7, crc), 2) == Reason::SEQUENCE_ACTIVE);
    // ABORT with nothing running.
    assert(rig.send(abortCommand(8), 2) == Reason::NOT_RUNNING);
  }

  // ---- ARMED buffer and Cancel during ARMED ----
  {
    Rig rig;
    assert(rig.send(start(0x1111, 7, rig.crc()), 1000) == Reason::NONE);
    assert(rig.controller->state() == State::ARMED && rig.controller->playingRun() == 0);
    assert(rig.controller->status(1000).remainingMs == 30000);
    assert(rig.send(start(0x2222, 7, rig.crc()), 1001) == Reason::ALREADY_RUNNING);
    assert(rig.controller->state() == State::ARMED);  // Rejection never alters a run.
    const Status s = rig.controller->status(1001);
    assert(s.reason == uint8_t(Reason::ALREADY_RUNNING) && s.commandToken == 0x2222 && s.runToken == 0x1111);
    for (uint32_t t = 1000; t < 31000; t += 5) rig.step(t);
    assert(rig.controller->state() == State::ARMED && rig.controller->status(30995).remainingMs == 5);
    rig.cancel = true;
    rig.step(30999);
    assert(rig.controller->state() == State::ABORTED && rig.count(Event::CANCEL) == 1);
    assert(rig.first(Event::CANCEL)->payload.stateOrReason == uint8_t(Reason::CANCELLED));
    assert(rig.count(Event::BUFFER_END) == 0 && rig.sequence.stage() == DemoSequence::Stage::IDLE);
    rig.cancel = false;
    assert(rig.send(start(0x3333, 7, rig.crc()), 31000) == Reason::NONE);  // Restartable.
  }

  // ---- Full pulse-track run: alert -> one inference sequence -> DONE ----
  {
    Rig rig;  // Rule model: deterministic for a pulse that stops.
    rig.source.script.irSamples = 12000;          // 120 s of script.
    rig.source.script.pulseStopsAtSample = 3000;  // Pulse stops 30 s into playback.
    rig.source.script.spo2Seconds = 120;
    assert(rig.send(start(0xbeef, 7, rig.crc()), 0) == Reason::NONE);
    uint32_t t = 0;
    for (; t < 30000; t += 5) rig.step(t);
    assert(rig.controller->state() == State::ARMED);
    rig.step(30000);
    assert(rig.controller->state() == State::PLAYING && rig.controller->playingRun() != 0);
    const uint32_t run = rig.controller->runId();
    for (t = 30005; t <= 30000 + 120000 + 200; t += 5) {
      rig.step(t);
      if (rig.controller->state() == State::DONE) break;
    }
    // Playback is 120 s; the inference sequence is 70 s and ends inside it.
    assert(rig.controller->state() == State::DONE);
    assert(rig.count(Event::STARTED) == 1 && rig.count(Event::BUFFER_END) == 1);
    assert(rig.count(Event::ALERT) == 1 && rig.count(Event::ACTUATION_START) == 1 &&
           rig.count(Event::ACTUATION_STOP) == 1 && rig.count(Event::DONE) == 1);
    assert(rig.count(Event::PULSE_RESULT) == 1 + (12000 - 1600) / 200);
    assert(rig.count(Event::OXYGEN_RESULT) == 120);
    const TimedEvent* alert = rig.first(Event::ALERT);
    const TimedEvent* act = rig.first(Event::ACTUATION_START);
    const TimedEvent* stop = rig.first(Event::ACTUATION_STOP);
    assert(alert->payload.flags & EV_PULSE_TRACK);
    // Pulse stops at playback 30 s; the rule needs a >= 4 s gap measured from
    // the last detected peak (just before the stop), on a 2-s cadence.
    const uint32_t alertPlayMs = alert->nowMs - 30000;
    assert(alertPlayMs >= 32000 && alertPlayMs <= 38000);
    assert(act->nowMs - alert->nowMs <= 10);         // Next control tick.
    const uint32_t outputMs = stop->nowMs - act->nowMs;
    assert(outputMs >= 70000 && outputMs <= 70010);  // 10 s vibration + 60 s pump.
    assert(alert->payload.runToken == 0xbeef && alert->payload.elapsedMs == alert->nowMs);
    // Exactly one inference sequence; the same run can never re-trigger.
    assert(!rig.sequence.requestInference(t, true, false, true, run, run));
    const Status s = rig.controller->status(t);
    assert(s.state == uint8_t(State::DONE) && (s.flags & PULSE_ALERT) && (s.flags & TRIGGER_ACCEPTED) &&
           (s.flags & PLAYBACK_FINISHED) && s.runToken == 0xbeef);
    std::cout << "pulse run: alert at playback " << alertPlayMs << " ms, outputs " << outputMs << " ms\n";
  }

  // ---- Oxygen-track alert; playback ends while actuating -> DONE after outputs ----
  {
    Rig rig;
    rig.source.script.irSamples = 0;
    rig.source.script.spo2Seconds = 40;
    rig.source.script.spo2DeclineFrom = 5;
    rig.source.script.spo2SlopePerS = 2.0f;
    assert(rig.send(start(9, 7, rig.crc()), 0) == Reason::NONE);
    uint32_t t = 0;
    for (; t <= 30000 + 40000 + 80000; t += 5) {
      rig.step(t);
      if (rig.controller->state() == State::DONE) break;
    }
    const TimedEvent* alert = rig.first(Event::ALERT);
    assert(alert && (alert->payload.flags & EV_OXYGEN_TRACK) && !(alert->payload.flags & EV_PULSE_TRACK));
    assert(rig.controller->state() == State::DONE && rig.count(Event::ACTUATION_STOP) == 1);
    // DONE only after the outputs stopped, although the 40-s script ended earlier.
    assert(rig.first(Event::DONE)->nowMs >= rig.first(Event::ACTUATION_STOP)->nowMs);
    assert(rig.first(Event::DONE)->nowMs > 30000 + 40000);
  }

  // ---- App ABORT: allowed before actuation, refused while outputs run ----
  {
    Rig rig;
    rig.source.script.irSamples = 12000;
    rig.source.script.pulseStopsAtSample = 2000;
    assert(rig.send(start(1, 7, rig.crc()), 0) == Reason::NONE);
    uint32_t t = 0;
    for (; rig.controller->state() != State::ACTUATING; t += 5) rig.step(t);
    assert(rig.send(abortCommand(2), t) == Reason::ACTUATING);
    assert(rig.controller->state() == State::ACTUATING && rig.sequence.stage() != DemoSequence::Stage::IDLE);
    // Physical Cancel always wins: outputs stop and the run aborts.
    rig.cancel = true;
    rig.step(t + 5);
    assert(rig.sequence.stage() == DemoSequence::Stage::IDLE);
    assert(rig.controller->state() == State::ABORTED);
    rig.cancel = false;
    for (uint32_t u = t + 10; u < t + 200000; u += 50) rig.step(u);
    assert(rig.count(Event::ACTUATION_START) == 1);  // Never re-triggered.

    Rig early;
    assert(early.send(start(3, 7, early.crc()), 0) == Reason::NONE);
    for (uint32_t u = 0; u <= 31000; u += 5) early.step(u);
    assert(early.controller->state() == State::PLAYING);
    assert(early.send(abortCommand(4), 31005) == Reason::NONE);
    assert(early.controller->state() == State::ABORTED);
    assert(early.events.back().payload.stateOrReason == uint8_t(Reason::APP_ABORT));
  }

  // ---- Cancel during PLAYING and ALERTED ----
  for (State target : {State::PLAYING, State::ALERTED}) {
    Rig rig;
    rig.batteryOk = true;
    rig.source.script.irSamples = 12000;
    rig.source.script.pulseStopsAtSample = 2000;
    assert(rig.send(start(1, 7, rig.crc()), 0) == Reason::NONE);
    uint32_t t = 0;
    // Hold the output sequence off (battery) so ALERTED is observable.
    if (target == State::ALERTED) rig.batteryOk = false;
    for (; rig.controller->state() != target; t += 5) rig.step(t);
    rig.cancel = true;
    rig.step(t);
    assert(rig.controller->state() == State::ABORTED && rig.controller->playingRun() == 0);
  }

  // ---- Trigger refused by the output interlock: logged, never retried ----
  {
    Rig rig;
    rig.source.script.irSamples = 6000;
    rig.source.script.pulseStopsAtSample = 2000;
    assert(rig.send(start(0x0a0a, 7, rig.crc()), 0) == Reason::NONE);
    for (uint32_t t = 0; t < 30000; t += 5) rig.step(t);
    rig.batteryOk = false;  // Battery drops before the alert.
    uint32_t t = 30000;
    for (; rig.controller->state() != State::DONE && t < 200000; t += 5) {
      rig.step(t);
      if (rig.count(Event::REJECTED)) rig.batteryOk = true;  // Recovers immediately after.
    }
    assert(rig.controller->state() == State::DONE);
    assert(rig.count(Event::ALERT) == 1 && rig.count(Event::REJECTED) == 1 &&
           rig.count(Event::ACTUATION_START) == 0);
    assert(rig.first(Event::REJECTED)->payload.stateOrReason == uint8_t(Reason::TRIGGER_REFUSED));
    assert(rig.sequence.stage() == DemoSequence::Stage::IDLE);
  }

  // ---- Status/event byte layout spot checks ----
  {
    Status s{};
    s.state = 2; s.reason = 0; s.commandToken = 0x0102; s.scenarioId = 9; s.flags = 0x21;
    s.remainingMs = 0x01020304; s.catalogCrc = 0xa1b2; s.pulseScore = 1234; s.oxygenConcern = 5678;
    s.runToken = 0xc3d4;
    uint8_t b[20];
    memcpy(b, &s, 20);
    const uint8_t expected[20] = {1, 1, 2, 0, 0x02, 0x01, 9, 0x21, 0x04, 0x03, 0x02, 0x01,
                                  0xb2, 0xa1, 0xd2, 0x04, 0x2e, 0x16, 0xd4, 0xc3};
    assert(memcmp(b, expected, 20) == 0);
  }
  std::cout << "Scenario parser, rejection rules, ARMED buffer, Cancel priority, ABORT, "
               "inference trigger, refusal and layout tests passed\n";
}
