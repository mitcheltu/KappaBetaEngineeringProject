#include "FirmwareShared.h"
#include "SensorHistory.h"
#include <esp_heap_caps.h>
#include <esp_timer.h>

// Allocated once at boot. Only the Arduino loop appends and transfers records.
History::Buffer historyBuffer;

uint16_t historyFlags() {
  return (historyBuffer.capacity() ? History::ALLOCATED : 0) |
         (historyBuffer.exhausted() ? History::SEQUENCE_EXHAUSTED : 0);
}
void initializeHistory() {
  const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  if (psramFound()) {
    for (size_t bytes = Config::HISTORY_BYTES; bytes >= 1024 * 1024; bytes /= 2) {
      if (heap_caps_get_free_size(caps) < bytes + Config::PSRAM_HEADROOM_BYTES) continue;
      auto* storage = static_cast<History::Record*>(heap_caps_aligned_alloc(16, bytes, caps));
      if (storage) { historyBuffer.reset(storage, bytes / sizeof(History::Record)); break; }
    }
  }
  logMessage(historyBuffer.capacity() ? LogLevel::INFO : LogLevel::WARN, "HISTORY",
      "capacity=%lu records (%lu bytes); PSRAM=%d; no ML reserve",
      static_cast<unsigned long>(historyBuffer.capacity()),
      static_cast<unsigned long>(historyBuffer.capacity() * sizeof(History::Record)), psramFound());
  if (!historyBuffer.capacity())
    logMessage(LogLevel::WARN, "HISTORY", "Live only: enable OPI PSRAM on an R8 board to record history");
}
void recordDepthSample() {
  SensorHistory::depth(historyBuffer, sensors, esp_timer_get_time(), millis());
}
void recordImuSample() {
  SensorHistory::imu(historyBuffer, sensors, esp_timer_get_time(), millis());
}
void recordPpgSample(uint64_t sampleTimeUs) {
  SensorHistory::ppg(historyBuffer, sensors, sampleTimeUs, millis());
}
void recordPpgGap() {
  SensorHistory::ppgGap(historyBuffer, esp_timer_get_time());
}
void recordSurfaceZero() {
  SensorHistory::surfaceZero(historyBuffer, sensors.surfacePa, esp_timer_get_time());
}
void recordSystemSample(const SystemPacket& packet) {
  SensorHistory::append(historyBuffer, History::Kind::SYSTEM, packet, esp_timer_get_time());
}
