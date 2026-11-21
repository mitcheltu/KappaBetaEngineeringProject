// Demo scenario playback: a compiled-in scripted scenario (ScenarioData.h) is played
// through the on-device models after a 30 s arm buffer. The fused decision (IMU gate AND
// pulse/oxygen alert) can only *request* the vibration -> pump sequence; the control task
// decides, and physical Cancel / double tap / Serial off / battery interlock stop it.
// The app and Serial can only ARM or ABORT (before actuation) a preloaded scenario.
#include "FirmwareShared.h"
#include "ScenarioController.h"
#include "ScenarioData.h"
#include <esp_timer.h>
#include <freertos/queue.h>

std::atomic<uint32_t> scenarioTriggerRequest{0}, scenarioPlayingRun{0};
std::atomic<uint32_t> scenarioTriggerAccepted{0}, scenarioTriggerRefused{0};
std::atomic<uint32_t> cancelEvents{0};
std::atomic<uint8_t> sequenceInfo{0};

namespace DemoScenario {
uint64_t clockUs() { return static_cast<uint64_t>(esp_timer_get_time()); }

// ~110 KB of fixed model workspace: static storage, never a task stack.
Scenario::CompiledSource source;
Scenario::ConcernLevelTrack oxygenTrack;
Scenario::Inference inference(oxygenTrack, clockUs);
Scenario::Controller controller(source, inference, Config::SCENARIO_ARM_BUFFER_MS);

struct QueuedCommand { Scenario::Command command; const char* origin; };
StaticQueue_t commandQueueDefinition, statusMailboxDefinition;
uint8_t commandQueueStorage[4 * sizeof(QueuedCommand)];
uint8_t statusMailboxStorage[sizeof(Scenario::Status)];
QueueHandle_t commandQueue = nullptr, statusMailbox = nullptr;
bool ready = false;

const char* stateName(Scenario::State s) {
  switch (s) {
    case Scenario::State::IDLE: return "IDLE";
    case Scenario::State::ARMED: return "ARMED";
    case Scenario::State::PLAYING: return "PLAYING";
    case Scenario::State::ALERTED: return "ALERTED";
    case Scenario::State::ACTUATING: return "ACTUATING";
    case Scenario::State::DONE: return "DONE";
    case Scenario::State::ABORTED: return "ABORTED";
    case Scenario::State::REJECTED: return "REJECTED";
  }
  return "?";
}

const char* reasonName(uint8_t r) {
  static const char* names[] = {"none", "battery not ready", "sequence active", "SOS held",
      "unknown scenario", "catalog mismatch", "already running", "not running", "malformed",
      "disabled", "Cancel held", "cancelled", "app abort", "actuating (use Cancel)", "trigger refused"};
  return r < sizeof(names) / sizeof(names[0]) ? names[r] : "?";
}

void logEvent(const Scenario::TimedEvent& e) {
  using Scenario::Event;
  const auto& p = e.payload;
  const float t = p.elapsedMs / 1000.0f;
  switch (static_cast<Event>(p.event)) {
    case Event::STARTED:
      logMessage(LogLevel::INFO, "SCENARIO", "#%u armed: %lu s to get in the water", p.scenarioId,
                 static_cast<unsigned long>(Config::SCENARIO_ARM_BUFFER_MS / 1000));
      break;
    case Event::BUFFER_END: logMessage(LogLevel::INFO, "SCENARIO", "#%u playback started (t=%.1f s)", p.scenarioId, t); break;
    case Event::IMU_RESULT:
      logMessage(LogLevel::DEBUG, "SCENARIO", "t=%.1f IMU score=%.6f gate=%s (%.1f ms)", t, p.value,
                 (p.flags & Scenario::EV_ALERT) ? "OPEN" : "closed", p.aux / 10.0f);
      break;
    case Event::PULSE_RESULT:
      logMessage(LogLevel::INFO, "SCENARIO", "t=%.1f pulse score=%.3f%s (%.1f ms)%s", t, p.value,
                 (p.flags & Scenario::EV_ALERT) ? " ALERT" : "", p.aux / 10.0f,
                 (p.flags & Scenario::EV_OVER_BUDGET) ? " OVER BUDGET" : "");
      break;
    case Event::OXYGEN_RESULT:
      logMessage(LogLevel::DEBUG, "SCENARIO", "t=%.1f SpO2=%.0f concern=%.3f%s", t,
                 p.aux == UINT16_MAX ? NAN : p.aux / 100.0f, p.value, (p.flags & Scenario::EV_ALERT) ? " ALERT" : "");
      break;
    case Event::ALERT:
      logMessage(LogLevel::INFO, "SCENARIO", "t=%.1f FUSED ALERT: IMU gate %s + %s%s", t,
                 (p.flags & Scenario::EV_IMU_TRACK) ? "open" : "n/a",
                 (p.flags & Scenario::EV_PULSE_TRACK) ? "pulse " : "", (p.flags & Scenario::EV_OXYGEN_TRACK) ? "oxygen" : "");
      break;
    case Event::ACTUATION_START: logMessage(LogLevel::INFO, "SCENARIO", "t=%.1f vibration -> pump sequence started by inference", t); break;
    case Event::ACTUATION_STOP: logMessage(LogLevel::INFO, "SCENARIO", "t=%.1f outputs finished", t); break;
    case Event::CANCEL: logMessage(LogLevel::INFO, "SCENARIO", "t=%.1f ended: %s", t, reasonName(p.stateOrReason)); break;
    case Event::DONE: logMessage(LogLevel::INFO, "SCENARIO", "#%u playback complete (t=%.1f s)", p.scenarioId, t); break;
    case Event::REJECTED: logMessage(LogLevel::WARN, "SCENARIO", "#%u refused: %s", p.scenarioId, reasonName(p.stateOrReason)); break;
  }
}

void task(void*) {
  TickType_t wake = xTaskGetTickCount();
  uint32_t seenCancels = cancelEvents.load();
  Scenario::State previousState = Scenario::State::IDLE;
  for (;;) {
    const uint32_t now = millis();
    Scenario::Inputs in;
    in.nowMs = now;
    in.enabled = Config::DEMO_SCENARIOS_ENABLED && controlReady && Config::ENABLE_ACTUATORS &&
                 pumpOutputReady.load();
    const uint16_t mv = batteryMv.load();
    in.batteryOk = mv != UINT16_MAX && mv >= Config::ACTUATOR_MIN_BATTERY_MV;
    // Accepted run id first, then the sequence state (control stores the reverse order).
    in.triggerAcceptedRun = scenarioTriggerAccepted.load();
    in.triggerRefusedRun = scenarioTriggerRefused.load();
    const uint8_t info = sequenceInfo.load();
    in.sequenceActive = (info & 0x0f) != static_cast<uint8_t>(DemoSequence::Stage::IDLE);
    in.sequenceTrigger = static_cast<DemoSequence::Trigger>(info >> 4);
    const uint32_t state = actuatorState.load();
    in.sosHeld = (state & 16) != 0;
    const uint32_t cancels = cancelEvents.load();
    in.cancel = (state & 8) != 0 || cancels != seenCancels;
    seenCancels = cancels;

    QueuedCommand queued{};
    while (xQueueReceive(commandQueue, &queued, 0) == pdTRUE) {
      const auto reason = controller.command(queued.command, in);
      logReply("SCENARIO", "%s %s #%u: %s", queued.origin,
               queued.command.opcode == static_cast<uint8_t>(Scenario::Opcode::START) ? "START" : "ABORT",
               queued.command.scenarioId, reason == Scenario::Reason::NONE ? "accepted" : reasonName(uint8_t(reason)));
    }
    controller.tick(in);
    if (const uint32_t run = controller.takeTriggerRequest()) scenarioTriggerRequest.store(run);
    scenarioPlayingRun.store(controller.playingRun());

    Scenario::TimedEvent event;
    while (controller.popEvent(event)) logEvent(event);
    if (controller.state() != previousState) {
      previousState = controller.state();
      logMessage(LogLevel::INFO, "SCENARIO", "state=%s", stateName(previousState));
    }
    const Scenario::Status status = controller.status(millis());
    xQueueOverwrite(statusMailbox, &status);
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(Config::SCENARIO_PERIOD_MS));
  }
}
}  // namespace DemoScenario

bool initializeScenario() {
  using namespace DemoScenario;
  commandQueue = xQueueCreateStatic(4, sizeof(QueuedCommand), commandQueueStorage, &commandQueueDefinition);
  statusMailbox = xQueueCreateStatic(1, sizeof(Scenario::Status), statusMailboxStorage, &statusMailboxDefinition);
  if (!commandQueue || !statusMailbox) return false;
  const Scenario::Status idle = controller.status(millis());
  xQueueOverwrite(statusMailbox, &idle);
  ready = Config::DEMO_SCENARIOS_ENABLED &&
          xTaskCreatePinnedToCore(task, "demo-scenario", Config::SCENARIO_STACK_BYTES, nullptr,
                                  Config::SCENARIO_PRIORITY, nullptr, Config::SCENARIO_CORE) == pdPASS;
  logMessage(ready ? LogLevel::INFO : LogLevel::WARN, "SCENARIO",
             "%s: %u compiled scenarios, catalog CRC %04X, arm buffer %lu s, fusion = IMU gate AND (pulse OR oxygen)",
             ready ? "ready" : "disabled", unsigned(source.count()), scenarioCatalogCrc(),
             static_cast<unsigned long>(Config::SCENARIO_ARM_BUFFER_MS / 1000));
  return ready;
}

bool queueScenarioCommand(const Scenario::Command& command, const char* origin) {
  using namespace DemoScenario;
  if (!commandQueue) return false;
  const QueuedCommand queued{command, origin};
  return xQueueSend(commandQueue, &queued, 0) == pdTRUE;
}

bool scenarioStatus(Scenario::Status& out) {
  return DemoScenario::statusMailbox && xQueuePeek(DemoScenario::statusMailbox, &out, 0) == pdTRUE;
}

uint16_t scenarioCatalogCrc() { return Scenario::catalogCrc(DemoScenario::source); }

size_t scenarioCatalog(uint8_t* out, size_t capacity) {
  using namespace DemoScenario;
  Scenario::CatalogHeader header;
  header.count = static_cast<uint8_t>(source.count());
  header.catalogCrc = scenarioCatalogCrc();
  const size_t needed = sizeof(header) + source.count() * sizeof(Scenario::CatalogEntry);
  if (!out || capacity < needed) return 0;
  memcpy(out, &header, sizeof(header));
  for (size_t i = 0; i < source.count(); ++i) {
    Scenario::CatalogEntry entry;
    source.descriptor(i, entry.descriptor);
    strncpy(entry.name, source.name(i), Scenario::NAME_BYTES - 1);
    memcpy(out + sizeof(header) + i * sizeof(entry), &entry, sizeof(entry));
  }
  return needed;
}

void logScenarioCatalog() {
  using namespace DemoScenario;
  for (size_t i = 0; i < source.count(); ++i) {
    Scenario::Descriptor d;
    source.descriptor(i, d);
    logReply("SCENARIO", "%u: %s (%lu s)", d.id, source.name(i), static_cast<unsigned long>(d.spo2Samples));
  }
  Scenario::Status s{};
  if (scenarioStatus(s))
    logReply("SCENARIO", "state=%s scenario=%u remaining=%lu ms gate=%d pulse=%d oxygen=%d trigger=%d (send: scenario <id> | scenario abort)",
             stateName(static_cast<Scenario::State>(s.state)), s.scenarioId, static_cast<unsigned long>(s.remainingMs),
             (s.flags & Scenario::IMU_GATE_OPEN) != 0, (s.flags & Scenario::PULSE_ALERT) != 0,
             (s.flags & Scenario::OXYGEN_ALERT) != 0, (s.flags & Scenario::TRIGGER_ACCEPTED) != 0);
}
