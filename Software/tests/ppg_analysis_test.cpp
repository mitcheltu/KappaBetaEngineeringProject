// Run the actual analysis task with a virtual clock/queue and deterministic
// estimator outputs. The task, ring, decimator, oxygen rule, quality gate and
// pulse inference are production code, not a reimplementation of their flow.
#include "../ppgSoftware/FirmwareShared.h"
#include <cassert>
#include <iostream>
#include <vector>

QueueHandle_t rawQueue = reinterpret_cast<void*>(1);
QueueHandle_t commandQueue = reinterpret_cast<void*>(2);
QueueHandle_t statusQueue = reinterpret_cast<void*>(3);
std::atomic<std::uint32_t> sampleGeneration{0}, queueDrops{0}, sensorGaps{0};
std::atomic<bool> sensorReady{true};

namespace {
std::uint64_t fakeUs = 0, sampleOrdinal = 0;
bool gapIssued = false;
bool lateInferenceInjected = false;
unsigned timerReadsSinceRaw = 0;
struct EndStream {};
struct Snapshot { std::uint64_t at; AnalysisStatus status; };
std::vector<Snapshot> snapshots;
std::vector<std::uint64_t> estimateTimes;
struct ScheduledCommand { std::uint64_t at; AnalysisCommand command; };
const ScheduledCommand commands[] = {
    {18000000, {CommandKind::Qualify, 1, 90}},
    {23000000, {CommandKind::Revoke, 0, 0}},
    {39500000, {CommandKind::Qualify, 1, 90}},
    {40000000, {CommandKind::Qualify, 2, 90}},
    {56500000, {CommandKind::Qualify, 2, 90}},
    {65000000, {CommandKind::Acknowledge, 0, 0}}
};
std::size_t commandIndex = 0;
std::uint32_t irAt(std::uint64_t ordinal) {
  return static_cast<std::uint32_t>(100000.0 +
      5000.0 * std::sin(double(ordinal) * 0.02 * 3.141592653589793));
}
}

std::uint32_t millis() { return static_cast<std::uint32_t>(fakeUs / 1000); }
std::int64_t esp_timer_get_time() {
  // After a pulse-window dequeue the task reads time for freshness, begin,
  // then finish. Advance the clock at finish to model a long computation or
  // preemption without replacing the production detector's result.
  if (++timerReadsSinceRaw == 3 && fakeUs >= 57000000 && !lateInferenceInjected) {
    fakeUs += 600000;
    lateInferenceInjected = true;
  }
  return static_cast<std::int64_t>(fakeUs);
}
int digitalRead(int pin) { assert(pin == Config::ACK_BUTTON); return HIGH; }
void rgbLedWrite(int pin, std::uint8_t, std::uint8_t, std::uint8_t) {
  assert(pin == Config::ALARM_LED_PIN); // No pump/actuator API is supplied.
}

int xQueueReceive(QueueHandle_t queue, void* destination, TickType_t wait) {
  if (queue == commandQueue) {
    assert(wait == 0);
    if (commandIndex < sizeof(commands) / sizeof(commands[0]) &&
        fakeUs >= commands[commandIndex].at) {
      *static_cast<AnalysisCommand*>(destination) = commands[commandIndex++].command;
      return pdTRUE;
    }
    return pdFALSE;
  }
  assert(queue == rawQueue && wait == pdMS_TO_TICKS(10));
  timerReadsSinceRaw = 0;
  fakeUs += 10000;
  if (fakeUs > 67000000) throw EndStream{};
  // Hold the producer for 1.1 s without a generation notification. At second
  // 11 the last Maxim estimate is still inside its 1.5 s freshness allowance,
  // but it must not count as another estimate toward median/confirmation.
  if (fakeUs >= 10100000 && fakeUs < 11200000) return pdFALSE;
  if (fakeUs >= 59000000 && !gapIssued) {
    gapIssued = true;
    sampleGeneration.fetch_add(1); sensorGaps.fetch_add(1);
    return pdFALSE;
  }
  const auto value = irAt(sampleOrdinal);
  const std::uint64_t arrival = fakeUs >= 21000000 && fakeUs < 22000000
      ? fakeUs - 600000 : fakeUs;
  *static_cast<RawSample*>(destination) = RawSample{
      sampleOrdinal++ * 10000, arrival, 90000, value, sampleGeneration.load()};
  return pdTRUE;
}

int xQueueOverwrite(QueueHandle_t queue, const void* value) {
  assert(queue == statusQueue);
  snapshots.push_back({fakeUs, *static_cast<const AnalysisStatus*>(value)});
  return pdTRUE;
}

void maxim_heart_rate_and_oxygen_saturation(std::uint32_t* ir, std::int32_t size,
    std::uint32_t* red, std::int32_t* spo2, std::int8_t* spo2Valid,
    std::int32_t* hr, std::int8_t* hrValid) {
  assert(size == 100);
  if (fakeUs <= 10000000) {
    // Initial 400 raw samples, then a 100-raw-sample/25-decimated-sample step.
    const auto start = (fakeUs - 4000000) / 10000;
    assert(fakeUs >= 4000000 && (fakeUs - 4000000) % 1000000 == 0);
    for (std::uint64_t i = 0; i < 100; ++i) {
      const auto expected = (std::uint64_t(irAt(start + i * 4)) + irAt(start + i * 4 + 1) +
          irAt(start + i * 4 + 2) + irAt(start + i * 4 + 3)) / 4;
      assert(ir[i] == expected && red[i] == 90000);
    }
  }
  estimateTimes.push_back(fakeUs);
  *spo2 = fakeUs < 60000000 ? 60 : 98;
  *spo2Valid = 1; *hr = 60; *hrValid = 1;
}

#include "../ppgSoftware/Analysis.ino"

namespace {
const AnalysisStatus& at(std::uint64_t time) {
  for (const auto& snap : snapshots)
    if (snap.at >= time && snap.at < time + 250000) return snap.status;
  assert(false && "Expected virtual-clock status snapshot missing");
  return snapshots.front().status;
}
}

int main() {
  try { analysisTask(nullptr); assert(false); } catch (const EndStream&) {}
  assert(!estimateTimes.empty() && estimateTimes.front() == 4000000);
  assert(estimateTimes[1] == 5000000); // Decimator/Maxim cadence: 4 s then 1 s.
  assert(at(10000000).oxygenActive);
  assert(at(11000000).oxygenActive && std::isnan(at(11000000).concern));
  assert(std::isfinite(at(19250000).pulseScore)); // Actual inference was run.
  assert(at(20250000).windowFull && at(20250000).qualified);
  assert(at(21250000).windowFull && !at(21250000).qualified);
  assert(at(22250000).qualified); // Freshness recovers without requalification.
  assert(!at(23250000).windowFull && !at(23250000).qualified);
  assert(at(23250000).oxygenActive); // Revoke resets samples, not an alert.
  assert(at(39750000).qualified && at(39750000).wearer == 1);
  assert(!at(40250000).windowFull && !at(40250000).qualified);
  assert(at(40250000).wearer == 2);
  assert(at(56750000).qualified);
  assert(lateInferenceInjected && at(58750000).analysisUs == 600000);
  assert(std::isnan(at(58750000).pulseScore)); // Refuse a late model result.
  assert(!at(59250000).windowFull && !at(59250000).qualified);
  assert(at(59250000).oxygenActive && at(59250000).sensorGaps == 1);
  assert(at(64750000).oxygenActive); // Recovery alone cannot clear a latch.
  assert(!at(65250000).oxygenActive && !at(65250000).pulseActive);
  std::cout << "PPG analysis scheduling and latch tests passed\n";
}
