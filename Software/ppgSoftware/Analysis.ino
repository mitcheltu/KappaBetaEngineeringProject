#include "FirmwareShared.h"

void updateIndicator(const AnalysisStatus& s) {
  // Red = latched research alert. Amber = unavailable/unqualified. Blue = ready.
  const bool alert = s.oxygenActive || s.pulseActive;
  const bool ready = sensorReady.load() && s.qualified && s.windowFull && s.spo2Valid &&
                     std::isfinite(s.pulseScore);
  const uint8_t color = alert ? 1 : ready ? 2 : 3;
  static uint8_t previous = 0;
  if (color == previous) return;
  previous = color;
  rgbLedWrite(Config::ALARM_LED_PIN, color == 1 ? 48 : color == 3 ? 24 : 0,
      color == 3 ? 12 : 0, color == 2 ? 24 : 0);
}

void analysisTask(void*) {
  // One owner for all DSP state. Large workspaces have static storage, not task stack.
  static Ppg::PpgWindow ring;
  static Ppg::PulseLoss detector;
  static float pulseCopy[Ppg::PpgWindow::kSamples];
  static uint32_t redWindow[Config::MAXIM_SAMPLES], irWindow[Config::MAXIM_SAMPLES];
  PpgDecimator<Config::DECIMATION> decimator;
  Ppg::OxygenConcern oxygen(Config::oxygenConfig());
  Ppg::QualityGate gate;
  AnalysisStatus status{};
  status.concern = status.pulseScore = NAN;
  size_t maximCount = 0, sincePulse = 0;
  uint64_t estimateUs = 0, lastRawUs = 0, lastEstimateUsedUs = 0;
  uint32_t generation = sampleGeneration.load(), lastSecond = UINT32_MAX;
  uint32_t lastPublish = 0;
  bool pulseLatched = false;
  bool ackRaw = digitalRead(Config::ACK_BUTTON) == LOW, ackStable = ackRaw;
  uint32_t ackChanged = millis();

  auto invalidate = [&]() {
    ring.reset(); decimator.reset(); maximCount = sincePulse = 0;
    estimateUs = 0; status.hrValid = status.spo2Valid = false;
    status.pulseScore = NAN;
    gate.revoke(); // A previous placement/continuous stream no longer qualifies.
  };

  for (;;) {
    uint64_t nowUs = static_cast<uint64_t>(esp_timer_get_time());
    const uint32_t currentGeneration = sampleGeneration.load();
    if (generation != currentGeneration) { generation = currentGeneration; invalidate(); }
    AnalysisCommand command{};
    while (xQueueReceive(commandQueue, &command, 0) == pdTRUE) {
      if (command.kind == CommandKind::Acknowledge) {
        oxygen.acknowledge(); pulseLatched = false;
      } else if (command.kind == CommandKind::Revoke) {
        invalidate(); // A placement/wearer change must collect a new window.
      } else {
        if (status.wearer && status.wearer != command.wearer) invalidate();
        status.wearer = command.wearer;
        // Serial qualification is an explicit operator assertion, not an inferred model result.
        if (sensorReady.load() && ring.full() && command.seconds <= Config::QUALIFICATION_MAX_SECONDS)
          gate.qualify(command.wearer, nowUs, uint64_t(command.seconds) * 1000000, true);
        else gate.revoke();
      }
    }
    const uint32_t nowMs = millis();
    const bool nextAck = digitalRead(Config::ACK_BUTTON) == LOW;
    if (nextAck != ackRaw) { ackRaw = nextAck; ackChanged = nowMs; }
    if (ackRaw != ackStable && nowMs - ackChanged >= 30) {
      ackStable = ackRaw;
      if (ackStable) { oxygen.acknowledge(); pulseLatched = false; }
    }

    RawSample raw{};
    if (xQueueReceive(rawQueue, &raw, pdMS_TO_TICKS(10)) == pdTRUE && raw.generation == generation) {
      ++status.samples;
      const bool contactValid = raw.ir >= Config::MIN_IR && raw.ir < Config::MAX_ADC &&
                                raw.red > 0 && raw.red < Config::MAX_ADC;
      const uint32_t ringGeneration = ring.generation();
      const float pulseIr = static_cast<float>(raw.ir) * (Config::PULSE_INVERT_IR ? -1.0f : 1.0f);
      ring.add(pulseIr, raw.timeUs, contactValid);
      lastRawUs = raw.arrivalUs;
      if (ring.generation() != ringGeneration) {
        decimator.reset(); maximCount = sincePulse = 0;
        status.hrValid = status.spo2Valid = false; estimateUs = 0;
        status.pulseScore = NAN; gate.revoke();
      }
      uint32_t red = 0, ir = 0;
      if (contactValid && decimator.add(raw.red, raw.ir, red, ir)) {
        redWindow[maximCount] = red; irWindow[maximCount++] = ir;
        if (maximCount == Config::MAXIM_SAMPLES) {
          int8_t validHr = 0, validSpo2 = 0;
          maxim_heart_rate_and_oxygen_saturation(irWindow, Config::MAXIM_SAMPLES, redWindow,
              &status.spo2, &validSpo2, &status.hr, &validHr);
          status.hrValid = validHr != 0 && status.hr > 0;
          status.spo2Valid = validSpo2 != 0 && status.spo2 > 0 && status.spo2 <= 100;
          estimateUs = raw.arrivalUs;
          memmove(redWindow, redWindow + 25, 75 * sizeof(uint32_t));
          memmove(irWindow, irWindow + 25, 75 * sizeof(uint32_t));
          maximCount = 75;
        }
      }
      if (contactValid) ++sincePulse;
      if (ring.full() && sincePulse >= Config::PULSE_STEP_SAMPLES) {
        sincePulse = 0;
        nowUs = static_cast<uint64_t>(esp_timer_get_time());
        const bool fresh = nowUs >= raw.arrivalUs && nowUs - raw.arrivalUs <= Config::PULSE_STALE_US;
        // Gate FIRST: it is independent of whether the current waveform contains beats.
        const bool permitted = gate.evaluate(fresh, nowUs, status.wearer).accepted;
        status.pulseScore = NAN;
        if (permitted && ring.copy(pulseCopy)) {
          const uint64_t begin = static_cast<uint64_t>(esp_timer_get_time());
          const auto result = detector.evaluate(pulseCopy, Config::PULSE_MODEL, Config::OPERATING_POINT);
          const uint64_t finished = static_cast<uint64_t>(esp_timer_get_time());
          status.analysisUs = static_cast<uint32_t>(finished - begin);
          if (status.analysisUs > status.worstAnalysisUs) status.worstAnalysisUs = status.analysisUs;
          if (status.analysisUs > Config::ANALYSIS_BUDGET_US) ++status.missedBudgets;
          // An overflow, expiry, or long calculation cannot publish a stale decision.
          if (result.valid && generation == sampleGeneration.load() &&
              finished >= raw.arrivalUs && finished - raw.arrivalUs <= Config::PULSE_STALE_US &&
              gate.evaluate(true, finished, status.wearer).accepted) {
            status.pulseScore = static_cast<float>(result.score);
            if (result.alert) pulseLatched = true;
          }
        }
      }
    }
    nowUs = static_cast<uint64_t>(esp_timer_get_time());
    if (lastRawUs && nowUs > lastRawUs && nowUs - lastRawUs > Config::ESTIMATE_STALE_US) {
      if (ring.count()) invalidate();
    }
    const uint32_t second = static_cast<uint32_t>(nowUs / 1000000);
    if (second != lastSecond) {
      lastSecond = second;
      const bool fresh = sensorReady.load() && generation == sampleGeneration.load() && estimateUs &&
          nowUs >= estimateUs && nowUs - estimateUs <= Config::ESTIMATE_STALE_US;
      // A still-fresh cached estimate is not another independent 1 Hz reading.
      const bool newEstimate = estimateUs != lastEstimateUsedUs;
      oxygen.update(static_cast<float>(status.spo2), fresh && newEstimate && status.spo2Valid, second);
      lastEstimateUsedUs = estimateUs;
      status.concern = oxygen.concern();
    }
    status.windowFull = ring.full();
    const bool pulseReady = sensorReady.load() && generation == sampleGeneration.load() &&
        ring.full() && lastRawUs && nowUs >= lastRawUs &&
        nowUs - lastRawUs <= Config::PULSE_STALE_US && Ppg::PulseLoss::available(Config::PULSE_MODEL);
    status.qualified = gate.evaluate(pulseReady, nowUs, status.wearer).accepted;
    status.oxygenActive = oxygen.active(); status.pulseActive = pulseLatched;
    status.queueDrops = queueDrops.load(); status.sensorGaps = sensorGaps.load();
    updateIndicator(status);
    if (millis() - lastPublish >= 250) {
      lastPublish = millis();
      xQueueOverwrite(statusQueue, &status); // Serial owner may block; analysis never does.
    }
  }
}
