#include "FirmwareShared.h"

void controlTask(void*) {
  BenchSequence sequence;
  uint32_t lastBattery = 0;
  bool sosRaw = digitalRead(Config::SOS_BUTTON) == LOW;
  bool sosStable = sosRaw; // A reed held at boot must not look like a new press.
  uint32_t sosChangedMs = millis();
  bool lastSos = sosStable, lastCancel = digitalRead(Config::CANCEL_BUTTON) == LOW;
  BatteryCondition lastCondition = BatteryCondition::UNKNOWN;
  uint32_t lastBatteryLog = 0, lastLoggedMv = UINT16_MAX;
  logMessage(LogLevel::INFO, "BUTTON", "boot SOS=%s Cancel=%s; SOS must be released before starting",
      sosStable ? "pressed" : "released", lastCancel ? "pressed" : "released");
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    const uint32_t now = millis();
    const bool cancel = digitalRead(Config::CANCEL_BUTTON) == LOW;
    const bool nextSos = digitalRead(Config::SOS_BUTTON) == LOW;
    const bool rawChanged = nextSos != sosRaw;
    if (rawChanged) { sosRaw = nextSos; sosChangedMs = now; }
    if (now - sosChangedMs >= 30) sosStable = sosRaw;
    if (now - lastBattery >= 100) {
      lastBattery = now;
      uint32_t sum = 0;
      for (int i = 0; i < 8; ++i) sum += analogReadMilliVolts(Config::BATTERY_ADC);
      const uint32_t mv = lroundf((sum / 8.0f) * Config::BATTERY_DIVIDER_RATIO);
      batteryMv.store(mv >= Config::BATTERY_VALID_MIN_MV && mv <= Config::BATTERY_VALID_MAX_MV
          ? static_cast<uint16_t>(mv) : UINT16_MAX);
      batteryDiagnostic.store((static_cast<uint32_t>(lroundf(sum / 8.0f)) << 16) | mv);
    }
    const uint16_t mv = batteryMv.load();
    const bool commandStop = stopRequested.exchange(false);
    const bool commandStart = startRequested.exchange(false);
    const bool stop = commandStop || cancel;
    const bool start = commandStart || sosStable;
    const auto oldStage = sequence.stage;
    const bool wasArmed = sequence.canAcceptSos();
    const uint32_t sample = batteryDiagnostic.load();
    const uint32_t estimatedMv = sample & 0xffff;
    const BatteryCondition condition = batteryCondition(estimatedMv);
    sequence.update(now, Config::ENABLE_ACTUATOR_TESTS, stop, start,
                    mv != UINT16_MAX && mv >= Config::ACTUATOR_MIN_BATTERY_MV, 0);
    digitalWrite(Config::VIBRATION_GATE, LOW); // Blue LED substitutes for the motor.
    digitalWrite(Config::PUMP_GATE, sequence.pumpOn() ? HIGH : LOW);
    // Addressable RGB LED: update only on countdown entry/exit, after the pump.
    // Setup has already turned it off before this task starts.
    const bool blueLedOn = sequence.vibrationOn();
    if (blueLedOn != (oldStage == BenchSequence::Stage::COUNTDOWN))
      rgbLedWrite(Config::COUNTDOWN_LED_PIN, 0, 0, blueLedOn ? Config::COUNTDOWN_LED_BLUE : 0);
    // Keep the legacy vibration flag (bit 5) for the countdown indicator.
    const uint8_t flags = (sequence.pumpOn() ? 4 : 0) | (cancel ? 8 : 0) |
        (sosStable ? 16 : 0) | (blueLedOn ? 32 : 0) |
        (sequence.stage == BenchSequence::Stage::COUNTDOWN ? 64 : 0);
    actuatorState.store((sequence.remainingCountdownMs(now) << 8) | flags);
    sosReady.store(sequence.canAcceptSos());
    // All event formatting/queueing happens AFTER output updates. The queue
    // never waits for space; only the lower-priority writer touches Serial.
    if (rawChanged) logMessage(LogLevel::DEBUG, "BUTTON", "SOS raw=%s; debounce=30 ms", sosRaw ? "LOW" : "HIGH");
    if (sosStable != lastSos) logMessage(LogLevel::INFO, "BUTTON", "SOS %s (debounced)", sosStable ? "PRESSED" : "RELEASED");
    if (cancel != lastCancel) logMessage(LogLevel::INFO, "BUTTON", "Cancel %s (immediate stop)", cancel ? "PRESSED" : "RELEASED");
    const uint32_t difference = estimatedMv > lastLoggedMv ? estimatedMv - lastLoggedMv : lastLoggedMv - estimatedMv;
    if (condition != lastCondition || (sample != UINT32_MAX && difference >= 50 && now - lastBatteryLog >= 500)) {
      logMessage(condition == BatteryCondition::READY ? LogLevel::INFO : LogLevel::WARN, "BATTERY",
          "GPIO%d=%u mV => battery=%lu mV; %s; SOS %s (allowed %u..%u mV)",
          Config::BATTERY_ADC, static_cast<unsigned>(sample >> 16),
          static_cast<unsigned long>(estimatedMv), batteryConditionName(condition),
          condition == BatteryCondition::READY ? "allowed after release" : "BLOCKED",
          Config::ACTUATOR_MIN_BATTERY_MV, Config::BATTERY_VALID_MAX_MV);
      lastCondition = condition; lastBatteryLog = now; lastLoggedMv = estimatedMv;
    }
    if (commandStart || (sosStable && !lastSos)) {
      const char* reason = !Config::ENABLE_ACTUATOR_TESTS ? "actuator tests disabled" :
          stop ? "Cancel/off has priority" : condition != BatteryCondition::READY ? batteryConditionName(condition) :
          oldStage != BenchSequence::Stage::IDLE ? "sequence already active" :
          !wasArmed ? "SOS not armed: release then press again" : nullptr;
      logMessage(reason ? LogLevel::WARN : LogLevel::INFO, "SOS", "%s %s%s",
          commandStart ? "serial request" : "button press", reason ? "REJECTED: " : "ACCEPTED", reason ? reason : "");
    }
    if (sequence.stage != oldStage) {
      const char* stage = sequence.stage == BenchSequence::Stage::COUNTDOWN ? "COUNTDOWN" :
          sequence.stage == BenchSequence::Stage::RUNNING ? "RUNNING" : "IDLE";
      const char* reason = stop ? "Cancel/off" : !Config::ENABLE_ACTUATOR_TESTS ? "disabled" :
          condition != BatteryCondition::READY ? batteryConditionName(condition) :
          sequence.stage == BenchSequence::Stage::COUNTDOWN ? "SOS accepted" : "countdown complete";
      logMessage(LogLevel::INFO, "ACTUATOR", "%s: blue_led=%d pump=%d; vibration=0; reason=%s",
          stage, blueLedOn, sequence.pumpOn(), reason);
    }
    if (commandStop && oldStage == BenchSequence::Stage::IDLE)
      logMessage(LogLevel::INFO, "ACTUATOR", "off processed; both outputs already off");
    if (!wasArmed && sequence.canAcceptSos())
      logMessage(LogLevel::INFO, "SOS", "armed after release; ready for a fresh press");
    lastSos = sosStable; lastCancel = cancel;
    // Independent of sensor/BLE blocking. Cancel acts on the first low level.
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(5));
  }
}
