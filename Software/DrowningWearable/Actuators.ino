#include "FirmwareShared.h"

void controlTask(void*) {
  BenchSequence sequence;
  uint32_t lastBattery = 0;
  bool sosRaw = digitalRead(Config::SOS_BUTTON) == LOW;
  bool sosStable = sosRaw; // A reed held at boot must not look like a new press.
  uint32_t sosChangedMs = millis();
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    const uint32_t now = millis();
    const bool cancel = digitalRead(Config::CANCEL_BUTTON) == LOW;
    const bool nextSos = digitalRead(Config::SOS_BUTTON) == LOW;
    if (nextSos != sosRaw) { sosRaw = nextSos; sosChangedMs = now; }
    if (now - sosChangedMs >= 30) sosStable = sosRaw;
    if (now - lastBattery >= 100) {
      lastBattery = now;
      uint32_t sum = 0;
      for (int i = 0; i < 8; ++i) sum += analogReadMilliVolts(Config::BATTERY_ADC);
      const uint32_t mv = lroundf((sum / 8.0f) * Config::BATTERY_DIVIDER_RATIO);
      batteryMv.store(mv >= 2500 && mv <= 4350 ? static_cast<uint16_t>(mv) : UINT16_MAX);
    }
    const uint16_t mv = batteryMv.load();
    const bool stop = stopRequested.exchange(false) || cancel;
    const bool start = startRequested.exchange(false) || sosStable;
    sequence.update(now, Config::ENABLE_ACTUATOR_TESTS, stop, start,
                    mv != UINT16_MAX && mv >= Config::ACTUATOR_MIN_BATTERY_MV, 0);
    digitalWrite(Config::VIBRATION_GATE, sequence.vibrationOn() ? HIGH : LOW);
    digitalWrite(Config::PUMP_GATE, sequence.pumpOn() ? HIGH : LOW);
    const uint8_t flags = (sequence.pumpOn() ? 4 : 0) | (cancel ? 8 : 0) |
        (sosStable ? 16 : 0) | (sequence.vibrationOn() ? 32 : 0) |
        (sequence.stage == BenchSequence::Stage::COUNTDOWN ? 64 : 0);
    actuatorState.store((sequence.remainingCountdownMs(now) << 8) | flags);
    // Independent of sensor/BLE blocking. Cancel acts on the first low level.
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(5));
  }
}
