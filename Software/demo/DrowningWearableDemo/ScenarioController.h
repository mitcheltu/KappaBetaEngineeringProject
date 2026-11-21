#pragma once
#include <cmath>
#include <stdint.h>
#include "DemoInference.h"
#include "DemoSequence.h"
#include "ScenarioProtocol.h"
#include "ScenarioSource.h"

// Single-owner scenario state machine (the analysis task on the device; virtual
// time in host tests). It never drives a GPIO. It can only *request* one
// inference trigger per run; DemoSequence (owned by the control task) decides,
// and physical Cancel / battery / disabled outputs still stop everything.
//
//   START ok -> ARMED (30 s buffer) -> PLAYING -> ALERTED -> ACTUATING -> DONE
//   Cancel/Serial off in any running state -> ABORTED
//   app ABORT before actuation -> ABORTED; START refused -> REJECTED (if idle)
namespace Scenario {
struct Inputs {
  uint32_t nowMs = 0;
  bool enabled = false;        // Scenarios compiled in AND control task/outputs available.
  bool batteryOk = false;      // Same interlock as the outputs.
  bool sequenceActive = false; // Vibration or pump phase running.
  DemoSequence::Trigger sequenceTrigger = DemoSequence::Trigger::NONE;
  bool sosHeld = false;
  bool cancel = false;         // Physical Cancel level OR a Serial off request.
  uint32_t triggerAcceptedRun = 0, triggerRefusedRun = 0;  // Control task replies.
};

class Controller {
 public:
  static constexpr size_t EVENT_CAPACITY = 32;
  static constexpr unsigned MAX_STEPS_PER_TICK = 8;

  Controller(Source& source, Inference& inference, uint32_t armBufferMs = 30000)
      : source_(source), inference_(inference), armBufferMs_(armBufferMs) {}

  // Returns the reason (NONE = accepted). Rejections never alter a running run.
  Reason command(const Command& c, const Inputs& in) {
    commandToken_ = c.token;
    Reason reason = Reason::NONE;
    if (c.opcode == static_cast<uint8_t>(Opcode::ABORT)) {
      if (!running(state_)) reason = Reason::NOT_RUNNING;
      else if (state_ == State::ACTUATING) reason = Reason::ACTUATING;
      else {
        finish(State::ABORTED, Event::CANCEL, Reason::APP_ABORT, in.nowMs);
        reason_ = Reason::NONE;
        return Reason::NONE;
      }
    } else {
      Descriptor d{};
      if (!in.enabled) reason = Reason::SCENARIOS_DISABLED;
      else if (running(state_)) reason = Reason::ALREADY_RUNNING;
      else if (in.cancel) reason = Reason::CANCEL_HELD;
      else if (!in.batteryOk) reason = Reason::BATTERY_NOT_READY;
      else if (in.sequenceActive) reason = Reason::SEQUENCE_ACTIVE;
      else if (in.sosHeld) reason = Reason::SOS_HELD;
      else if (!findScenario(source_, c.scenarioId, d) || !source_.select(c.scenarioId))
        reason = Reason::UNKNOWN_SCENARIO;
      else if (c.catalogCrc != catalogCrc(source_)) reason = Reason::CATALOG_MISMATCH;
      if (reason == Reason::NONE) {
        if (++runId_ == 0) runId_ = 1;
        runToken_ = c.token;
        scenarioId_ = c.scenarioId;
        startedMs_ = in.nowMs;
        playStartMs_ = 0;
        played_ = false;
        triggerRequested_ = triggerPending_ = triggerAccepted_ = false;
        actuated_ = false;
        reason_ = Reason::NONE;
        inference_.stop();
        state_ = State::ARMED;
        emit(in.nowMs, Event::STARTED, NAN, UINT16_MAX, 0, static_cast<uint8_t>(state_));
        return reason;
      }
    }
    reason_ = reason;
    if (!running(state_)) state_ = State::REJECTED;
    emit(in.nowMs, Event::REJECTED, NAN, c.token, 0, static_cast<uint8_t>(reason), c.scenarioId);
    return reason;
  }

  void tick(const Inputs& in) {
    if (!running(state_)) return;
    if (in.cancel) {
      finish(State::ABORTED, Event::CANCEL, Reason::CANCELLED, in.nowMs);
      return;
    }
    if (state_ == State::ARMED) {
      if (in.nowMs - startedMs_ < armBufferMs_) return;
      playStartMs_ = in.nowMs;
      played_ = true;
      inference_.begin(&source_);
      state_ = State::PLAYING;
      emit(in.nowMs, Event::BUFFER_END, NAN, UINT16_MAX, 0, static_cast<uint8_t>(state_));
    }
    // Inference keeps consuming the script through alert and actuation, so the
    // log records the whole run.
    const uint32_t playElapsed = in.nowMs - playStartMs_;
    for (unsigned i = 0; i < MAX_STEPS_PER_TICK; ++i) {
      const Step step = inference_.advance(playElapsed);
      if (!step.pulse && !step.oxygen && !step.imu) break;
      if (step.imu) {
        const ImuOutcome& m = step.imuOutcome;
        const uint32_t tenths = m.analysisUs / 100u;
        emit(in.nowMs, Event::IMU_RESULT, m.score,
             static_cast<uint16_t>(tenths > 0xfffeu ? 0xfffeu : tenths),
             uint8_t((m.gateOpen ? EV_ALERT : 0) | (m.valid ? EV_VALID : 0) | EV_IMU_TRACK),
             static_cast<uint8_t>(state_));
      }
      if (step.pulse) {
        const PulseOutcome& p = step.pulseOutcome;
        const uint32_t tenths = p.analysisUs / 100u;
        emit(in.nowMs, Event::PULSE_RESULT, p.score,
             static_cast<uint16_t>(tenths > 0xfffeu ? 0xfffeu : tenths),
             uint8_t((p.alert ? EV_ALERT : 0) | (p.valid ? EV_VALID : 0) | EV_PULSE_TRACK |
                     (p.overBudget ? EV_OVER_BUDGET : 0)),
             static_cast<uint8_t>(state_));
      }
      if (step.oxygen) {
        const OxygenOutcome& o = step.oxygenOutcome;
        const float spo2 = o.spo2 * 100.0f;
        emit(in.nowMs, Event::OXYGEN_RESULT, o.concern,
             std::isfinite(spo2) && spo2 >= 0 && spo2 < 65535 ? static_cast<uint16_t>(std::lround(spo2)) : UINT16_MAX,
             uint8_t((o.active ? EV_ALERT : 0) | (o.valid ? EV_VALID : 0) | EV_OXYGEN_TRACK),
             static_cast<uint8_t>(state_));
      }
    }
    if (state_ == State::PLAYING && inference_.alert()) {
      state_ = State::ALERTED;
      triggerPending_ = true;
      emit(in.nowMs, Event::ALERT, NAN, UINT16_MAX,
           uint8_t((inference_.pulseActive() ? EV_PULSE_TRACK : 0) |
                   (inference_.oxygenActive() ? EV_OXYGEN_TRACK : 0) |
                   (inference_.imuGateOpen() ? EV_IMU_TRACK : 0)),
           static_cast<uint8_t>(state_));
    }
    if (triggerPending_ && triggerRequested_) {
      if (in.triggerAcceptedRun == runId_) {
        triggerPending_ = false;
        triggerAccepted_ = true;
      } else if (in.triggerRefusedRun == runId_) {
        triggerPending_ = false;
        emit(in.nowMs, Event::REJECTED, NAN, runToken_, 0,
             static_cast<uint8_t>(Reason::TRIGGER_REFUSED), scenarioId_);
      }
    }
    const bool inferenceRunning = in.sequenceActive &&
                                  in.sequenceTrigger == DemoSequence::Trigger::INFERENCE;
    if (state_ == State::ALERTED && triggerAccepted_ && !actuated_ && inferenceRunning) {
      state_ = State::ACTUATING;
      actuated_ = true;
      emit(in.nowMs, Event::ACTUATION_START, NAN, UINT16_MAX, 0, static_cast<uint8_t>(state_));
    } else if (state_ == State::ACTUATING && !inferenceRunning) {
      state_ = State::ALERTED;  // Alert stays latched; outputs finished.
      emit(in.nowMs, Event::ACTUATION_STOP, NAN, UINT16_MAX, 0, static_cast<uint8_t>(state_));
    }
    // Wait only for the control task's one-shot reply. The firmware publishes
    // the sequence state before the accepted run id, and loads them in the
    // opposite order, so an accepted trigger is already visible as running.
    if (inference_.finished() && state_ != State::ACTUATING && !triggerPending_)
      finish(State::DONE, Event::DONE, Reason::NONE, in.nowMs);
  }

  // One-shot request for the control task (nonzero run id), then cleared.
  uint32_t takeTriggerRequest() {
    if (!triggerPending_ || triggerRequested_) return 0;
    triggerRequested_ = true;
    return runId_;
  }
  // Run id while playback may legitimately trigger outputs (else 0).
  uint32_t playingRun() const {
    return state_ == State::PLAYING || state_ == State::ALERTED ? runId_ : 0;
  }

  bool popEvent(TimedEvent& out) {
    if (!eventCount_) return false;
    out = events_[eventHead_];
    eventHead_ = (eventHead_ + 1) % EVENT_CAPACITY;
    --eventCount_;
    return true;
  }
  uint32_t droppedEvents() const { return droppedEvents_; }

  Status status(uint32_t nowMs) const {
    Status s{};
    s.state = static_cast<uint8_t>(state_);
    s.reason = static_cast<uint8_t>(reason_);
    s.commandToken = commandToken_;
    s.scenarioId = scenarioId_;
    s.runToken = runToken_;
    s.catalogCrc = catalogCrc(source_);
    s.remainingMs = remainingMs(nowMs);
    const bool playback = played_ && state_ != State::ARMED && state_ != State::REJECTED;
    if (playback) {
      s.pulseScore = scaleUnit(inference_.lastPulseScore());
      s.oxygenConcern = scaleUnit(inference_.concern());
    }
    s.flags = uint8_t((playback && inference_.pulseActive() ? PULSE_ALERT : 0) |
                      (playback && inference_.oxygenActive() ? OXYGEN_ALERT : 0) |
                      (triggerAccepted_ ? TRIGGER_ACCEPTED : 0) |
                      (s.pulseScore != UINT16_MAX ? PULSE_VALID : 0) |
                      (s.oxygenConcern != UINT16_MAX ? CONCERN_VALID : 0) |
                      SCENARIOS_COMPILED |
                      (playback && inference_.finished() ? PLAYBACK_FINISHED : 0) |
                      (playback && inference_.imuGateOpen() ? IMU_GATE_OPEN : 0));
    return s;
  }

  State state() const { return state_; }
  uint32_t runId() const { return runId_; }

 private:
  uint32_t remainingMs(uint32_t nowMs) const {
    if (state_ == State::ARMED) {
      const uint32_t elapsed = nowMs - startedMs_;
      return elapsed < armBufferMs_ ? armBufferMs_ - elapsed : 0;
    }
    if (state_ == State::PLAYING || state_ == State::ALERTED || state_ == State::ACTUATING) {
      const uint64_t irMs = uint64_t(source_.irCount()) * 10u;
      const uint64_t spo2Ms = uint64_t(source_.spo2Count()) * 1000u;
      const uint64_t total = irMs > spo2Ms ? irMs : spo2Ms;
      const uint64_t elapsed = nowMs - playStartMs_;
      const uint64_t left = elapsed < total ? total - elapsed : 0;
      return left > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(left);
    }
    return 0;
  }

  void finish(State next, Event event, Reason reason, uint32_t nowMs) {
    inference_.stop();
    triggerPending_ = false;
    state_ = next;
    reason_ = reason;
    emit(nowMs, event, NAN, UINT16_MAX, 0,
         static_cast<uint8_t>(event == Event::CANCEL ? static_cast<uint8_t>(reason)
                                                     : static_cast<uint8_t>(state_)));
  }

  void emit(uint32_t nowMs, Event event, float value, uint16_t aux, uint8_t flags,
            uint8_t stateOrReason, uint8_t scenarioId = 0xff) {
    TimedEvent e{};
    e.nowMs = nowMs;
    e.payload.event = static_cast<uint8_t>(event);
    e.payload.scenarioId = scenarioId == 0xff ? scenarioId_ : scenarioId;
    e.payload.runToken = event == Event::REJECTED ? aux : runToken_;
    e.payload.elapsedMs = nowMs - startedMs_;
    e.payload.value = value;
    e.payload.aux = event == Event::REJECTED ? UINT16_MAX : aux;
    e.payload.flags = flags;
    e.payload.stateOrReason = stateOrReason;
    if (eventCount_ == EVENT_CAPACITY) {  // Drop the oldest; count it.
      eventHead_ = (eventHead_ + 1) % EVENT_CAPACITY;
      --eventCount_;
      ++droppedEvents_;
    }
    events_[(eventHead_ + eventCount_) % EVENT_CAPACITY] = e;
    ++eventCount_;
  }

  Source& source_;
  Inference& inference_;
  const uint32_t armBufferMs_;
  State state_ = State::IDLE;
  Reason reason_ = Reason::NONE;
  uint16_t commandToken_ = 0, runToken_ = 0;
  uint8_t scenarioId_ = 0;
  uint32_t runId_ = 0, startedMs_ = 0, playStartMs_ = 0;
  bool triggerPending_ = false, triggerRequested_ = false, triggerAccepted_ = false;
  bool actuated_ = false, played_ = false;
  TimedEvent events_[EVENT_CAPACITY];
  size_t eventHead_ = 0, eventCount_ = 0;
  uint32_t droppedEvents_ = 0;
};
}  // namespace Scenario
