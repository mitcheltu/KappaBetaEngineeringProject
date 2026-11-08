#include "FirmwareShared.h"
#include <stdarg.h>
#include <freertos/queue.h>
#include <esp_system.h>

namespace Diagnostics {
constexpr size_t LINE_BYTES = 224;
constexpr size_t QUEUE_LENGTH = 48;
struct Line { char text[LINE_BYTES]; };
StaticQueue_t queueDefinition;
uint8_t queueStorage[QUEUE_LENGTH * sizeof(Line)];
QueueHandle_t queue = nullptr;
TaskHandle_t writerHandle = nullptr;
std::atomic<int> level{Config::DEFAULT_LOG_LEVEL};
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
  const uint32_t battery = batteryDiagnostic.load();
  const uint32_t controls = actuatorState.load();
  if (battery == UINT32_MAX) logReply("BATTERY", "NOT_SAMPLED; SOS blocked until a valid battery sample");
  else logReply("BATTERY", "GPIO%d=%u mV x %.2f => %u mV; %s; allowed=%u..%u mV",
      Config::BATTERY_ADC, static_cast<unsigned>(battery >> 16), Config::BATTERY_DIVIDER_RATIO,
      static_cast<unsigned>(battery & 0xffff), batteryConditionName(batteryCondition(battery & 0xffff)),
      Config::ACTUATOR_MIN_BATTERY_MV, Config::BATTERY_VALID_MAX_MV);
  logReply("CONTROL", "task=%s enabled=%d SOS=%s Cancel=%s armed=%d stage=%s blue_led=%d pump=%d countdown=%lu ms; vibration=0",
      controlReady ? "ready" : "FAILED", Config::ENABLE_ACTUATOR_TESTS,
      (controls & 16) ? "pressed" : "released", (controls & 8) ? "pressed" : "released",
      sosReady.load(), (controls & 64) ? "COUNTDOWN" : (controls & 4) ? "RUNNING" : "IDLE",
      !!(controls & 32), !!(controls & 4), static_cast<unsigned long>(controls >> 8));
  logReply("SENSORS", "I2C=%d BMI270 ready/fresh=%d/%d MS5837=%d/%d MAX30102=%d/%d errors=%lu FIFO_gaps=%lu",
      busReady, imuReady, imuValid && now - imuMs < 300,
      depthReady, pressureValid && now - pressureMs < 1000,
      ppgReady, rawPpgValid && now - ppgMs < 300,
      static_cast<unsigned long>(sensorErrors), static_cast<unsigned long>(ppgOverflows));
  logReply("SYSTEM", "heap=%lu min_heap=%lu PSRAM_free=%lu BLE=%s live=%d history_flags=0x%04x log_dropped=%lu truncated=%lu",
      static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(ESP.getMinFreeHeap()),
      static_cast<unsigned long>(ESP.getFreePsram()),
      historyConnectionHandle() == BLE_HS_CONN_HANDLE_NONE ? "disconnected" : "connected",
      historyLiveEnabled(), historyFlags(), static_cast<unsigned long>(Diagnostics::dropped.load()),
      static_cast<unsigned long>(Diagnostics::truncated.load()));
  logReply("STACK", "minimum free bytes: control=%lu logger=%lu (0 if task unavailable)",
      static_cast<unsigned long>(controlTaskHandle ? uxTaskGetStackHighWaterMark(controlTaskHandle) : 0),
      static_cast<unsigned long>(Diagnostics::writerHandle ? uxTaskGetStackHighWaterMark(Diagnostics::writerHandle) : 0));
  // Values stay in the main-loop owner; the writer task never reads sensor state.
  logMessage(LogLevel::DEBUG, "VALUES", "pressure=%.1f Pa temp=%.2f C zero=%.1f Pa; IR=%lu red=%lu HR=%ld(valid=%d) SpO2=%ld(valid=%d)",
      pressurePa, temperatureC, surfacePa, static_cast<unsigned long>(irValue),
      static_cast<unsigned long>(redValue), static_cast<long>(heartRate), hrValid,
      static_cast<long>(oxygenSaturation), spo2Valid);
  logMessage(LogLevel::DEBUG, "IMU", "last sample age=%lu ms; axes=[%.3f %.3f %.3f %.3f %.3f %.3f] valid=%d",
      static_cast<unsigned long>(now - imuMs), imuAxes[0], imuAxes[1], imuAxes[2],
      imuAxes[3], imuAxes[4], imuAxes[5], imuValid);
}

void serviceDiagnostics(uint32_t now) {
  static uint32_t lastStatus = 0, lastDropped = 0;
  if (now - lastStatus < Config::LOG_STATUS_PERIOD_MS) return;
  lastStatus = now;
  const uint32_t lost = Diagnostics::dropped.load();
  if (lost != lastDropped) {
    logMessage(LogLevel::WARN, "LOG", "%lu lines dropped (total=%lu); reduce log level or drain Serial",
        static_cast<unsigned long>(lost - lastDropped), static_cast<unsigned long>(lost));
    lastDropped = lost;
  }
  if (Diagnostics::level.load() >= static_cast<int>(LogLevel::INFO)) logStatus();
}
