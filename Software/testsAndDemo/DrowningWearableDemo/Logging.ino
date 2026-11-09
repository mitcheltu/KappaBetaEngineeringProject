#include "FirmwareShared.h"
#include <stdarg.h>
#include <freertos/queue.h>
#include <esp_system.h>

namespace Diagnostics {
constexpr size_t LINE_BYTES = 224;
constexpr size_t QUEUE_LENGTH = 16;
struct Line { char text[LINE_BYTES]; };
StaticQueue_t queueDefinition;
uint8_t queueStorage[QUEUE_LENGTH * sizeof(Line)];
QueueHandle_t queue = nullptr;
TaskHandle_t writerHandle = nullptr;
std::atomic<int> level{2};
std::atomic<uint32_t> dropped{0}, truncated{0};

const char* resetName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "POWER_ON";
    case ESP_RST_EXT: return "EXTERNAL_RESET";
    case ESP_RST_SW: return "SOFTWARE_RESET";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INTERRUPT_WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK_WATCHDOG";
    case ESP_RST_WDT: return "WATCHDOG";
    case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO_RESET";
    default: return "OTHER/UNKNOWN";
  }
}

void enqueue(LogLevel severity, const char* tag, const char* format, va_list args) {
  if (!queue) return;
  Line line{};
  const char* names[] = {"ERROR", "WARN", "INFO", "DEBUG"};
  const int prefix = snprintf(line.text, sizeof(line.text), "[%10lu ms] %-5s %-8.8s ",
      static_cast<unsigned long>(millis()), names[static_cast<uint8_t>(severity)], tag);
  if (prefix < 0 || static_cast<size_t>(prefix) >= sizeof(line.text) - 2) return;
  const size_t space = sizeof(line.text) - prefix - 1; // Reserve newline + NUL.
  const int written = vsnprintf(line.text + prefix, space, format, args);
  if (written < 0) return;
  size_t end = strlen(line.text);
  if (static_cast<size_t>(written) >= space) {
    truncated.fetch_add(1);
    memcpy(line.text + end - 3, "...", 3);
  }
  line.text[end++] = '\n';
  line.text[end] = '\0';
  // Never wait on a UART, USB connection or a full log queue in a producer.
  if (xQueueSend(queue, &line, 0) != pdTRUE) dropped.fetch_add(1);
}

void writerTask(void*) {
  Line line{};
  size_t offset = 0, length = 0;
  for (;;) {
    if (offset == length) {
      if (xQueueReceive(queue, &line, pdMS_TO_TICKS(20)) != pdTRUE) continue;
      offset = 0; length = strlen(line.text);
    }
    // Sole firmware Serial writer. Keep partial lines intact and isolate any
    // driver backpressure in this lower-priority task, never the control task.
    const int room = Serial.availableForWrite();
    if (room > 0) {
      const size_t pending = length - offset;
      const size_t count = pending < static_cast<size_t>(room) ? pending : room;
      offset += Serial.write(reinterpret_cast<const uint8_t*>(line.text + offset), count);
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
} // namespace Diagnostics

bool initializeLogging() {
  Diagnostics::queue = xQueueCreateStatic(Diagnostics::QUEUE_LENGTH, sizeof(Diagnostics::Line),
      Diagnostics::queueStorage, &Diagnostics::queueDefinition);
  if (!Diagnostics::queue) return false;
  if (xTaskCreate(Diagnostics::writerTask, "serial-log", 4096, nullptr, 1, &Diagnostics::writerHandle) != pdPASS) {
    Diagnostics::queue = nullptr;
    return false;
  }
  logMessage(LogLevel::INFO, "BOOT", "reset_reason=%s(%d); serial=115200; log level=%d; status every %lu ms",
      Diagnostics::resetName(esp_reset_reason()), static_cast<int>(esp_reset_reason()), Diagnostics::level.load(),
      static_cast<unsigned long>(Config::LOG_STATUS_PERIOD_MS));
  return true;
}

void logMessage(LogLevel severity, const char* tag, const char* format, ...) {
  if (static_cast<int>(severity) > Diagnostics::level.load()) return;
  va_list args;
  va_start(args, format);
  Diagnostics::enqueue(severity, tag, format, args);
  va_end(args);
}

void logReply(const char* tag, const char* format, ...) {
  va_list args;
  va_start(args, format);
  Diagnostics::enqueue(LogLevel::INFO, tag, format, args);
  va_end(args);
}

void setLogLevel(int level) {
  Diagnostics::level.store(level);
  logReply("LOG", "level=%s (0=ERROR, 1=WARN, 2=INFO, 3=DEBUG)",
      level < 0 ? "OFF" : level == 0 ? "0" : level == 1 ? "1" : level == 2 ? "2" : "3");
}

void logStatus() {
  const uint32_t now = millis();
  const uint32_t battery = batteryDiagnostic.load(), controls = actuatorState.load();
  if (battery == UINT32_MAX) logReply("BATTERY", "Not sampled; outputs blocked");
  else logReply("BATTERY", "ADC=%u mV battery=%u mV; required 3400..4350 mV",
      unsigned(battery >> 16), unsigned(battery & 0xffff));
  logReply("CONTROL", "task=%s stage=%s vibration=%d pump=%d remaining=%lu ms SOS=%d Cancel=%d armed=%d",
      controlReady ? "ready" : "FAILED", (controls & 64) ? "VIBRATING" : (controls & 4) ? "PUMPING" : "IDLE",
      !!(controls & 32), !!(controls & 4), static_cast<unsigned long>(controls >> 8),
      !!(controls & 16), !!(controls & 8), sosReady.load());
  logReply("SENSORS", "I2C=%d BMI270 ready/fresh=%d/%d MS5837=%d/%d MAX30102=%d/%d errors=%lu PPG_gaps=%lu",
      sensors.busReady, sensors.imuReady, sensors.imuValid && now - sensors.imuMs < 300,
      sensors.depthReady, sensors.pressureValid && now - sensors.pressureMs < 1000,
      sensors.ppgReady, sensors.rawPpgValid && now - sensors.ppgMs < 300,
      static_cast<unsigned long>(sensors.sensorErrors), static_cast<unsigned long>(sensors.ppgOverflows));
  logReply("STREAM", "BLE=%s live=%d; live-only demo (no history/model); log_dropped=%lu truncated=%lu",
      bleConnected() ? "connected" : "disconnected", bleLiveEnabled(),
      static_cast<unsigned long>(Diagnostics::dropped.load()), static_cast<unsigned long>(Diagnostics::truncated.load()));
  logMessage(LogLevel::DEBUG, "VALUES", "pressure=%.1fPa zero=%.1fPa temperature=%.2fC red=%lu IR=%lu HR=%ld(valid=%d) SpO2=%ld(valid=%d)",
      sensors.pressurePa, sensors.surfacePa, sensors.temperatureC,
      static_cast<unsigned long>(sensors.red), static_cast<unsigned long>(sensors.ir),
      static_cast<long>(sensors.heartRate), sensors.hrValid,
      static_cast<long>(sensors.oxygenSaturation), sensors.spo2Valid);
}

void serviceDiagnostics(uint32_t now) {
  static uint32_t lastStatus = 0;
  if (now - lastStatus < Config::LOG_STATUS_PERIOD_MS) return;
  lastStatus = now;
  if (Diagnostics::level.load() >= static_cast<int>(LogLevel::INFO)) logStatus();
}
