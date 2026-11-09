// Compiles the actual acquisition tab against fake I2C/RTOS boundaries.
#include "../ppgSoftware/FirmwareShared.h"
#include <cassert>
#include <deque>
#include <iostream>

TwoWire Wire;
QueueHandle_t rawQueue = reinterpret_cast<void*>(1), commandQueue = nullptr, statusQueue = nullptr;
std::atomic<std::uint32_t> sampleGeneration{0}, queueDrops{0}, sensorGaps{0};
std::atomic<bool> sensorReady{false};

namespace {
std::uint64_t fakeUs = 1000000;
std::deque<RawSample> delivered;
std::size_t queueCapacity = 512;
struct EndIteration {};
}

std::uint32_t millis() { return static_cast<std::uint32_t>(fakeUs / 1000); }
std::int64_t esp_timer_get_time() { return static_cast<std::int64_t>(fakeUs); }
void vTaskDelay(TickType_t ticks) { fakeUs += ticks * 1000; throw EndIteration{}; }
int xQueueSend(QueueHandle_t queue, const void* sample, TickType_t wait) {
  assert(queue == rawQueue && wait == 0);  // FIFO owner must never wait.
  if (delivered.size() >= queueCapacity) return pdFALSE;
  delivered.push_back(*static_cast<const RawSample*>(sample));
  return pdTRUE;
}

#include "../ppgSoftware/Ppg.ino"

namespace {
void resetFixture() {
  Wire = TwoWire{};
  sensor = MAX30105{};
  fakeUs = 1000000;
  lastSampleUs = 0;
  lastServiceMs = lastDataMs = millis();
  delivered.clear(); queueCapacity = 512;
  sampleGeneration.store(0); queueDrops.store(0); sensorGaps.store(0);
  sensorReady.store(true);
}

void iterateSensor() {
  try { sensorTask(nullptr); assert(false); } catch (const EndIteration&) {}
}

void addSample(std::uint32_t red, std::uint32_t ir) {
  Wire.fifo.push_back({static_cast<std::uint8_t>(red >> 16),
      static_cast<std::uint8_t>(red >> 8), static_cast<std::uint8_t>(red),
      static_cast<std::uint8_t>(ir >> 16), static_cast<std::uint8_t>(ir >> 8),
      static_cast<std::uint8_t>(ir)});
  Wire.registers[0x04] = (Wire.registers[0x04] + 1) & 0x1f;
}

void registerTransferFailures() {
  resetFixture();
  std::uint8_t bytes[3]{};
  Wire.shortReadReg = 0x04;
  assert(!readPpgRegisters(0x04, bytes, 3));
  assert(Wire.available() == 0);  // Short response drained.
  Wire.shortReadReg = -1; Wire.endStatus = 2;
  assert(!readPpgRegisters(0x04, bytes, 3));
  assert(!writePpgRegister(0x04, 0));
  Wire.endStatus = 0; Wire.writeOk = false;
  assert(!readPpgRegisters(0x04, bytes, 3));
  assert(!writePpgRegister(0x04, 0));
}

void configurationReadback() {
  resetFixture();
  assert(initializePpg());
  assert(sensor.average == 1 && sensor.mode == 2 && sensor.rate == 100);
  assert(sensor.width == 411 && sensor.range == 4096);
  assert(sampleGeneration.load() == 1 && sensorGaps.load() == 1);
  sensor.badAveragingReadback = true;
  assert(!initializePpg());
  sensor.badAveragingReadback = false; sensor.beginOk = false;
  assert(!initializePpg());
}

void timestampsAndQueuePressure() {
  resetFixture(); sampleGeneration.store(7);
  addSample(0xf12345, 0xf23456); addSample(90001, 100001); addSample(90002, 100002);
  iterateSensor();
  assert(delivered.size() == 3);
  assert(delivered[0].red == (0xf12345 & 0x3ffff));
  assert(delivered[0].ir == (0xf23456 & 0x3ffff));
  for (std::size_t i = 0; i < 3; ++i) {
    assert(delivered[i].timeUs == 980000 + 10000 * i);
    assert(delivered[i].arrivalUs == delivered[i].timeUs);
    assert(delivered[i].generation == 7);
  }
  fakeUs = 1017000; addSample(90003, 100003); iterateSensor();
  assert(delivered.back().timeUs == 1010000); // Nominal continuity clock.
  assert(delivered.back().arrivalUs == 1017000); // Host freshness clock.
  queueCapacity = delivered.size();
  acceptPpgSample(1, 2, 1020000, 1027000);
  assert(queueDrops.load() == 1 && sampleGeneration.load() == 8);
  delivered.clear();
  acceptPpgSample(1, 2, 1030000, 1037000);
  assert(delivered.front().generation == 8);
}

void discontinuities() {
  resetFixture();
  Wire.registers[0x05] = 1; addSample(90000, 100000);
  iterateSensor();
  assert(delivered.empty());
  assert(sampleGeneration.load() == 1 && sensorGaps.load() == 1);
  assert(Wire.registers[0x04] == 0 && Wire.registers[0x05] == 0 && Wire.registers[0x06] == 0);

  resetFixture();
  lastServiceMs = millis() - Config::FIFO_DEADLINE_MS;
  addSample(90000, 100000); iterateSensor();
  assert(delivered.empty() && sampleGeneration.load() == 1);

  resetFixture();
  Wire.failReadReg = 0x04; iterateSensor();
  assert(!sensorReady.load() && sampleGeneration.load() == 1);

  resetFixture();
  Wire.failReadReg = 0x07; addSample(90000, 100000); iterateSensor();
  assert(delivered.empty() && !sensorReady.load() && sampleGeneration.load() == 1);

  resetFixture();
  lastDataMs = millis() - Config::NO_SAMPLE_MS;
  iterateSensor();
  assert(!sensorReady.load() && sampleGeneration.load() == 1);
}
}  // namespace

int main() {
  registerTransferFailures(); configurationReadback();
  timestampsAndQueuePressure(); discontinuities();
  std::cout << "PPG acquisition hardware-boundary tests passed\n";
}
