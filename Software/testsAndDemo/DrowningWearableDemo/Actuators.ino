#include "FirmwareShared.h"

void controlTask(void*) {
  DemoSequence sequence;
  uint32_t lastBattery = 0;
  bool sosRaw = digitalRead(Config::SOS_BUTTON) == LOW;
  bool sosStable = sosRaw;
  uint32_t sosChangedMs = millis();
  bool previousCancel = false, previousSos = sosStable;
  bool previousBatteryOk = false;
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    uint32_t now = millis();
    bool cancel = digitalRead(Config::CANCEL_BUTTON) == LOW;
    // Stop the outputs immediately on the raw Cancel level, before ADC work.
    if (cancel) {
      digitalWrite(Config::VIBRATION_GATE, LOW);
      digitalWrite(Config::PUMP_GATE, LOW);
    }
    const bool nextSos = digitalRead(Config::SOS_BUTTON) == LOW;
    if (nextSos != sosRaw) { sosRaw = nextSos; sosChangedMs = now; }
    if (now - sosChangedMs >= Config::SOS_DEBOUNCE_MS) sosStable = sosRaw;
    if (now - lastBattery >= Config::BATTERY_PERIOD_MS) {
      lastBattery = now;
      uint32_t sum = 0;
      for (uint8_t i = 0; i < 8; ++i) sum += analogReadMilliVolts(Config::BATTERY_ADC);
      const uint32_t adcMv = (sum + 4) / 8;
      const uint32_t estimate = lroundf(adcMv * Config::BATTERY_DIVIDER_RATIO);
      batteryDiagnostic.store((adcMv << 16) | (estimate & 0xffff));
      batteryMv.store(estimate >= Config::BATTERY_VALID_MIN_MV &&
                      estimate <= Config::BATTERY_VALID_MAX_MV ? estimate : UINT16_MAX);
    }
    // Recheck after ADC reads; the physical button always has priority.
    cancel = cancel || digitalRead(Config::CANCEL_BUTTON) == LOW;
    const bool serialStop = stopRequested.exchange(false);
    now = millis();
    const uint16_t mv = batteryMv.load();
    const bool batteryOk = mv != UINT16_MAX && mv >= Config::ACTUATOR_MIN_BATTERY_MV;
    const auto oldStage = sequence.stage();
    sequence.update(now, Config::ENABLE_ACTUATORS, cancel || serialStop, sosStable, batteryOk);
    const bool motorOn = sequence.motorOn(now), pumpOn = sequence.pumpOn();
    // Motor off is written before pump on at the phase transition.
    digitalWrite(Config::VIBRATION_GATE, motorOn ? HIGH : LOW);
    digitalWrite(Config::PUMP_GATE, pumpOn ? HIGH : LOW);
    const uint8_t flags = (pumpOn ? 4 : 0) | (cancel ? 8 : 0) |
        (sosStable ? 16 : 0) | (motorOn ? 32 : 0) |
        (sequence.stage() == DemoSequence::Stage::VIBRATING ? 64 : 0);
    actuatorState.store((sequence.remainingPhaseMs(now) << 8) | flags);
    sosReady.store(sequence.canAcceptSos());
    // Logging is queued without waiting, after output changes.
    if (oldStage != sequence.stage())
      logMessage(LogLevel::INFO, "CONTROL", "stage=%s motor=%d pump=%d remaining=%lu ms",
          sequence.stage() == DemoSequence::Stage::VIBRATING ? "VIBRATING" :
          sequence.stage() == DemoSequence::Stage::PUMPING ? "PUMPING" : "IDLE",
          motorOn, pumpOn, static_cast<unsigned long>(sequence.remainingPhaseMs(now)));
    if (batteryOk != previousBatteryOk)
      logMessage(batteryOk ? LogLevel::INFO : LogLevel::WARN, "BATTERY",
          "estimate=%u mV; outputs %s (required 3400..4350 mV)",
          unsigned(batteryDiagnostic.load() & 0xffff), batteryOk ? "allowed" : "blocked/stopped");
    if (cancel != previousCancel || sosStable != previousSos)
      logMessage(LogLevel::INFO, "BUTTON", "SOS=%d Cancel=%d armed=%d battery_ok=%d",
          sosStable, cancel, sequence.canAcceptSos(), batteryOk);
    previousCancel = cancel;
    previousSos = sosStable;
    previousBatteryOk = batteryOk;
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(Config::CONTROL_PERIOD_MS));
  }
}
