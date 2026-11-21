#include "FirmwareShared.h"

void controlTask(void*) {
  BenchSequence sequence;
  bool pumpPwmAttached = false;
  bool haveHandledRiskRound = false;
  bool automaticEpisodeLatched = false;
  uint32_t lastHandledRiskRoundId = 0;
  uint32_t lastBattery = 0;
  bool sosRaw = digitalRead(Config::SOS_BUTTON) == LOW;
  bool sosStable = sosRaw; // A reed held at boot must not look like a new press.
  uint32_t sosChangedMs = millis();
  bool lastSos = sosStable, lastCancel = digitalRead(Config::CANCEL_BUTTON) == LOW;
  BatteryCondition lastCondition = BatteryCondition::UNKNOWN;
  uint32_t lastBatteryLog = 0, lastLoggedMv = UINT16_MAX;

  // Runtime actuator writes stay in this task. External pulldowns hold both
  // drivers off until these initial writes run.
  digitalWrite(Config::VIBRATION_GATE, LOW);
  digitalWrite(Config::PUMP_GATE, LOW);
  logMessage(LogLevel::INFO, "BUTTON", "boot SOS=%s Cancel=%s; SOS must be released before starting",
      sosStable ? "pressed" : "released", lastCancel ? "pressed" : "released");

  auto stopPumpPwm = [&]() {
    bool stopped = true;
    if (pumpPwmAttached) {
      const bool dutyCleared = ledcWrite(Config::PUMP_GATE, 0);
      const bool detached = ledcDetach(Config::PUMP_GATE);
      pumpPwmAttached = false;
      stopped = dutyCleared && detached;
    }
    digitalWrite(Config::PUMP_GATE, LOW);
    return stopped;
  };

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
    const bool batteryOk = mv != UINT16_MAX && mv >= Config::ACTUATOR_MIN_BATTERY_MV;

    // Consume each completed fusion round at most once. A fresh closed-gate
    // round rearms the next episode; repeated high rounds cannot restart a
    // sequence. This state never cancels an already-running manual/auto phase.
    bool automaticQualifyingRound = false;
    bool automaticRearmed = false;
    uint32_t automaticRoundId = 0;
    float automaticRisk = NAN;
    RiskRoundResult latestRisk{};
    if (riskRoundStore.trySnapshot(latestRisk) &&
        (!haveHandledRiskRound || latestRisk.roundId != lastHandledRiskRoundId)) {
      haveHandledRiskRound = true;
      lastHandledRiskRoundId = latestRisk.roundId;
      const uint64_t nowUs = static_cast<uint64_t>(esp_timer_get_time());
      const bool riskFresh = latestRisk.completedAtUs != 0 &&
          nowUs >= latestRisk.completedAtUs &&
          nowUs - latestRisk.completedAtUs <=
              static_cast<uint64_t>(Config::RISK_RESULT_STALE_AFTER_MS) * 1000ULL;
      if (riskFresh && !latestRisk.gateOpen) {
        automaticRearmed = automaticEpisodeLatched;
        automaticEpisodeLatched = false;
      } else if (riskFresh && Config::FUSION_TRIGGER_THRESHOLD_CONFIGURED &&
          latestRisk.gateOpen && latestRisk.fusionValid &&
          std::isfinite(latestRisk.combinedRisk) &&
          latestRisk.combinedRisk >= Config::FUSION_TRIGGER_THRESHOLD &&
          !automaticEpisodeLatched) {
        automaticEpisodeLatched = true;
        automaticQualifyingRound = true;
        automaticRoundId = latestRisk.roundId;
        automaticRisk = latestRisk.combinedRisk;
      }
    }
    autoTriggerEpisodeLatched.store(automaticEpisodeLatched);

    sequence.update(now, Config::ENABLE_ACTUATOR_TESTS, stop, start, batteryOk);
    const bool automaticStarted = automaticQualifyingRound &&
        sequence.startAutomatically(now, Config::ENABLE_ACTUATOR_TESTS, stop, batteryOk);

    bool pwmFailure = false;
    uint32_t pumpDuty = 0;
    if (sequence.stage == BenchSequence::Stage::PUMP_RAMP) {
      if (!pumpPwmAttached) {
        pumpPwmAttached = ledcAttach(Config::PUMP_GATE,
            Config::PUMP_PWM_FREQUENCY_HZ, Config::PUMP_PWM_RESOLUTION_BITS);
        if (!pumpPwmAttached || !ledcWrite(Config::PUMP_GATE, 0)) {
          pwmFailure = true;
          sequence.abort();
          stopPumpPwm();
          logMessage(LogLevel::ERROR, "PWM",
              "Could not start GPIO%d PWM at %lu Hz; sequence aborted and pump held LOW",
              Config::PUMP_GATE,
              static_cast<unsigned long>(Config::PUMP_PWM_FREQUENCY_HZ));
        }
      }
      if (sequence.stage == BenchSequence::Stage::PUMP_RAMP) {
        pumpDuty = sequence.pumpDuty(now);
        if (!ledcWrite(Config::PUMP_GATE, pumpDuty)) {
          pwmFailure = true;
          sequence.abort();
          stopPumpPwm();
          pumpDuty = 0;
          logMessage(LogLevel::ERROR, "PWM",
              "Could not update GPIO%d PWM duty; sequence aborted and pump held LOW",
              Config::PUMP_GATE);
        }
      }
    }

    if (sequence.stage == BenchSequence::Stage::PUMP_RUNNING) {
      if (pumpPwmAttached) {
        if (!stopPumpPwm()) {
          pwmFailure = true;
          sequence.abort();
          logMessage(LogLevel::ERROR, "PWM",
              "Could not stop GPIO%d PWM at ramp completion; sequence aborted and pump held LOW",
              Config::PUMP_GATE);
        }
      }
      if (sequence.stage == BenchSequence::Stage::PUMP_RUNNING) {
        digitalWrite(Config::PUMP_GATE, HIGH);
        pumpDuty = Config::PUMP_PWM_MAX_DUTY;
      }
    } else if (sequence.stage != BenchSequence::Stage::PUMP_RAMP) {
      if (!stopPumpPwm()) {
        pwmFailure = true;
        logMessage(LogLevel::ERROR, "PWM",
            "Could not cleanly disable GPIO%d PWM; pump output commanded LOW",
            Config::PUMP_GATE);
      }
      pumpDuty = 0;
    }

    digitalWrite(Config::VIBRATION_GATE,
        sequence.vibrationOn() ? HIGH : LOW);

    const uint8_t dutyPercent = static_cast<uint8_t>(
        (pumpDuty * 100 + Config::PUMP_PWM_MAX_DUTY / 2) /
        Config::PUMP_PWM_MAX_DUTY);
    actuatorDetails.store((static_cast<uint16_t>(dutyPercent) << 8) |
        static_cast<uint8_t>(sequence.stage));

    // Keep packet v1 meanings: pump command, Cancel, SOS, vibration,
    // vibration-phase countdown, and real-motor capability.
    const uint8_t flags = (sequence.pumpOn() ? 4 : 0) | (cancel ? 8 : 0) |
        (sosStable ? 16 : 0) | (sequence.vibrationOn() ? 32 : 0) |
        (sequence.stage == BenchSequence::Stage::VIBRATING ? 64 : 0) | 128;
    actuatorState.store((sequence.remainingVibrationMs(now) << 8) | flags);
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
    if (automaticRearmed)
      logMessage(LogLevel::INFO, "AUTO", "fresh closed-gate round observed; next qualifying episode may start a sequence");
    if (automaticQualifyingRound) {
      if (automaticStarted) {
        logMessage(LogLevel::INFO, "AUTO",
            "round=%lu fresh fusion=%.3f threshold=%.3f; actuator sequence accepted",
            static_cast<unsigned long>(automaticRoundId), automaticRisk,
            Config::FUSION_TRIGGER_THRESHOLD);
      } else {
        const char* reason = stop ? "Cancel/off has priority" :
            !Config::ENABLE_ACTUATOR_TESTS ? "actuator tests disabled" :
            !batteryOk ? batteryConditionName(condition) :
            sequence.stage != BenchSequence::Stage::IDLE ? "sequence already active" :
            "automatic start rejected by an interlock";
        logMessage(LogLevel::WARN, "AUTO",
            "round=%lu qualified and latched; start suppressed: %s",
            static_cast<unsigned long>(automaticRoundId), reason);
      }
    }
    if (sequence.stage != oldStage) {
      const char* reason = pwmFailure ? "PWM failure; outputs stopped" :
          stop ? "Cancel/off" : !Config::ENABLE_ACTUATOR_TESTS ? "disabled" :
          condition != BatteryCondition::READY ? batteryConditionName(condition) :
          sequence.stage == BenchSequence::Stage::VIBRATING ?
              (automaticStarted ? "fusion trigger accepted" : "SOS accepted") :
          sequence.stage == BenchSequence::Stage::PUMP_RAMP ? "vibration complete" :
          sequence.stage == BenchSequence::Stage::PUMP_RUNNING ? "pump ramp complete" :
          "sequence stopped";
      logMessage(LogLevel::INFO, "ACTUATOR",
          "%s: vibration=%d pump_command=%d duty=%u%%; reason=%s",
          BenchSequence::stageName(sequence.stage), sequence.vibrationOn(),
          sequence.pumpOn(), static_cast<unsigned>(dutyPercent), reason);
    }
    if (commandStop && oldStage == BenchSequence::Stage::IDLE)
      logMessage(LogLevel::INFO, "ACTUATOR", "off processed; vibration and pump already off");
    if (!wasArmed && sequence.canAcceptSos())
      logMessage(LogLevel::INFO, "SOS", "armed after release; ready for a fresh press");
    lastSos = sosStable; lastCancel = cancel;
    // Independent of sensor/BLE blocking. Cancel acts on the first low level.
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(5));
  }
}
