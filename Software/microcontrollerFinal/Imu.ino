#include "FirmwareShared.h"

void initializeImu() {
  static bool failureReported = false;
  logMessage(LogLevel::DEBUG, "IMU", "Initializing BMI270 at 0x%02X", unsigned(Config::IMU_ADDRESS));
  imuValid = false;
  imuReady = imu.begin(Wire, Config::IMU_ADDRESS);
  if (imuReady) {
    logMessage(LogLevel::INFO, "IMU", "BMI270 ready: accel +/-8g, gyro +/-2000dps, 100Hz sensor rate; firmware samples at 20Hz");
    failureReported = false;
  } else {
    ++sensorErrors;
    const LogLevel level = failureReported ? LogLevel::DEBUG : LogLevel::WARN;
    uint8_t chipId = 0;
    if (!readRegisters(Config::IMU_ADDRESS, 0x00, &chipId, 1)) {
      logMessage(level, "IMU", "BMI270 init failed at 0x%02X: ID read failed; check power, address and I2C wiring", unsigned(Config::IMU_ADDRESS));
    } else if (chipId != 0x24) {
      logMessage(level, "IMU", "BMI270 init failed: reg 0x00=0x%02X, expected ID 0x24; MPU6050 is not compatible", unsigned(chipId));
    } else {
      logMessage(level, "IMU", "BMI270 ID OK but initialization/configuration failed; retry pending");
    }
    failureReported = true;
  }
}

void sampleImu() {
  const bool wasReady = imuReady.load();
  const bool transferOk = wasReady && imu.update();
  imuValid = transferOk;
  ImuReading bufferedReading{};
  if (imuValid) {
    const ImuReading& reading = imu.reading();
    bufferedReading = reading;
    imuAxes[0] = reading.acceleration[0];
    imuAxes[1] = reading.acceleration[1];
    imuAxes[2] = reading.acceleration[2];
    imuAxes[3] = reading.gyro[0];
    imuAxes[4] = reading.gyro[1];
    imuAxes[5] = reading.gyro[2];
    imuMs = reading.timestampMs;
  } else {
    for (float& axis : imuAxes) axis = NAN;
    bufferedReading.timestampUs = static_cast<uint64_t>(esp_timer_get_time());
    bufferedReading.timestampMs = millis();
    bufferedReading.valid = false;
    if (wasReady) {
      logMessage(LogLevel::WARN, "IMU", "BMI270 sample read failed; readings invalid until reinitialization");
      ++sensorErrors;
      imuReady = false;
    }
  }

  // Keep one slot per 20 Hz attempt, including invalid reads, in the rolling
  // acquisition buffer. Processing copies its own stable window later.
  sensorBuffers.pushImu(bufferedReading);
  if (sensorBuffers.imuCount() >= ImuRisk::IMU_WINDOW_SAMPLES &&
      processingTaskHandle) {
    xTaskNotify(processingTaskHandle, PROCESSING_NOTIFY_IMU, eSetBits);
  }

  recordImuSample(bufferedReading.timestampUs);
}

bool calibrateImuGyro() {
  if (!imuReady.load()) return false;
  bool expected = false;
  return gyroCalibrationRequested.compare_exchange_strong(expected, true);
}

void clearImuCalibration() {
  gyroCalibrationClearRequested.store(true);
}

void processingTask(void*) {
  static ImuReading snapshot[ImuRisk::IMU_WINDOW_SAMPLES]{};
  static PpgReading ppgSnapshot[Config::PPG_ANALYSIS_WINDOW_READINGS]{};
  static bool imuTimingGapReported = false;
  struct CnnEvaluation {
    ImuRisk::EvaluationResult result{};
    bool inferenceRan = false;
  };

  CnnRiskGate gate(Config::CNN_SCORE_THRESHOLD);
  bool cachedCnnAvailable = false;
  ImuRisk::EvaluationResult cachedCnnResult{};
  uint64_t cachedCnnStartUs = 0, cachedCnnEndUs = 0;
  uint32_t nextRoundId = 0;
  uint64_t lastRoundTargetUs = 0;
  uint64_t openingTargetUs = 0;
  bool openingRoundPending = false;

  auto evaluateCnnAt = [&](uint64_t targetUs) -> CnnEvaluation {
    CnnEvaluation evaluation{};
    if (targetUs == 0) return evaluation;

    const std::size_t copied = sensorBuffers.copyLatestImu(
        targetUs, snapshot, ImuRisk::IMU_WINDOW_SAMPLES);
    if (copied != ImuRisk::IMU_WINDOW_SAMPLES) {
      imuRiskEvaluator.invalidate(0, targetUs);
      evaluation.result = imuRiskEvaluator.result();
      return evaluation;
    }

    imuWindow.clear();
    for (std::size_t i = 0; i < copied; ++i) imuWindow.push(snapshot[i]);
    const uint64_t inputStartUs = imuWindow.startTimestampUs();
    const uint64_t inputEndUs = imuWindow.endTimestampUs();
    if (!imuWindow.allValid()) {
      imuRiskEvaluator.invalidate(inputStartUs, inputEndUs);
      evaluation.result = imuRiskEvaluator.result();
      return evaluation;
    }
    if (!imuWindow.timingValid()) {
      imuRiskEvaluator.invalidate(inputStartUs, inputEndUs);
      evaluation.result = imuRiskEvaluator.result();
      if (!imuTimingGapReported) {
        logMessage(LogLevel::WARN, "IMU",
            "CNN window rejected: timing gap/span exceeds limits (span=%llu us max_gap=%llu us)",
            static_cast<unsigned long long>(imuWindow.durationUs()),
            static_cast<unsigned long long>(imuWindow.maxSampleGapUs()));
        imuTimingGapReported = true;
      }
      return evaluation;
    }
    if (imuTimingGapReported) {
      logMessage(LogLevel::INFO, "IMU", "CNN window timing recovered; inference resumed.");
      imuTimingGapReported = false;
    }
    if (targetUs < inputEndUs ||
        targetUs - inputEndUs > Config::RISK_ROUND_MAX_IMU_ENDPOINT_SKEW_US) {
      imuRiskEvaluator.invalidate(inputStartUs, inputEndUs);
      evaluation.result = imuRiskEvaluator.result();
      return evaluation;
    }

    if (cachedCnnAvailable && cachedCnnStartUs == inputStartUs &&
        cachedCnnEndUs == inputEndUs) {
      evaluation.result = cachedCnnResult;
      return evaluation;
    }

    imuRiskEvaluator.evaluate(imuWindow.tensor(), inputStartUs, inputEndUs);
    evaluation.result = imuRiskEvaluator.result();
    evaluation.inferenceRan = true;
    cachedCnnAvailable = true;
    cachedCnnResult = evaluation.result;
    cachedCnnStartUs = inputStartUs;
    cachedCnnEndUs = inputEndUs;

    const ImuReading& latest = snapshot[copied - 1];
    const float accelerationMagnitude = sqrtf(
        latest.acceleration[0] * latest.acceleration[0] +
        latest.acceleration[1] * latest.acceleration[1] +
        latest.acceleration[2] * latest.acceleration[2]);
    const float gyroMagnitude = sqrtf(
        latest.gyro[0] * latest.gyro[0] +
        latest.gyro[1] * latest.gyro[1] +
        latest.gyro[2] * latest.gyro[2]);
    logMessage(LogLevel::DEBUG, "IMU", "window=%u/%u interval=%llu..%llu us span=%llu us score=%.3f valid=%u accel=%.3fg gyro=%.3fdps",
        static_cast<unsigned>(copied), static_cast<unsigned>(ImuRisk::IMU_WINDOW_SAMPLES),
        static_cast<unsigned long long>(inputStartUs),
        static_cast<unsigned long long>(inputEndUs),
        static_cast<unsigned long long>(imuWindow.durationUs()),
        evaluation.result.score, static_cast<unsigned>(evaluation.result.valid),
        accelerationMagnitude, gyroMagnitude);
    return evaluation;
  };

  auto applyGate = [&](const CnnEvaluation& evaluation) -> GateTransition {
    if (!evaluation.inferenceRan) return GateTransition::NONE;
    const GateTransition transition = gate.observe(evaluation.result);
    if (transition == GateTransition::OPENED) {
      logMessage(LogLevel::INFO, "GATE",
          "OPEN score=%.3f threshold=%.3f CNN input=%llu..%llu us",
          evaluation.result.score, Config::CNN_SCORE_THRESHOLD,
          static_cast<unsigned long long>(evaluation.result.inputStartUs),
          static_cast<unsigned long long>(evaluation.result.inputEndUs));
    } else if (transition == GateTransition::CLOSED) {
      logMessage(LogLevel::INFO, "GATE",
          "CLOSED after two valid negative CNN rounds; latest score=%.3f input=%llu..%llu us",
          evaluation.result.score,
          static_cast<unsigned long long>(evaluation.result.inputStartUs),
          static_cast<unsigned long long>(evaluation.result.inputEndUs));
    }
    return transition;
  };

  auto runRiskRound = [&](uint64_t targetUs) -> bool {
    if (!gate.isOpen() || targetUs == 0 || targetUs <= lastRoundTargetUs)
      return false;

    const std::size_t ppgCopied = sensorBuffers.copyLatestPpg(
        targetUs, ppgSnapshot, Config::PPG_ANALYSIS_WINDOW_READINGS);
    if (ppgCopied != Config::PPG_ANALYSIS_WINDOW_READINGS) return false;

    bool ppgWindowValid = true;
    for (std::size_t i = 0; i < ppgCopied; ++i) {
      if (!ppgSnapshot[i].valid ||
          !ppgSignalAccepted(ppgSnapshot[i].red, ppgSnapshot[i].infrared) ||
          (i != 0 && ppgSnapshot[i].timestampUs <= ppgSnapshot[i - 1].timestampUs)) {
        ppgWindowValid = false;
        break;
      }
    }
    const uint64_t ppgStartUs = ppgSnapshot[0].timestampUs;
    const uint64_t ppgEndUs = ppgSnapshot[ppgCopied - 1].timestampUs;
    if (targetUs < ppgEndUs || targetUs - ppgEndUs >
            Config::RISK_ROUND_MAX_PPG_ENDPOINT_SKEW_US ||
        ppgEndUs < ppgStartUs ||
        ppgEndUs - ppgStartUs > Config::RISK_ROUND_MAX_PPG_WINDOW_SPAN_US ||
        (targetUs > Config::RISK_ROUND_MAX_PPG_WINDOW_SPAN_US &&
         ppgStartUs < targetUs - Config::RISK_ROUND_MAX_PPG_WINDOW_SPAN_US)) {
      ppgWindowValid = false;
    }
    // A full, recent PPG window anchors both the first round and the later
    // approximately one-second cadence. Do not queue an obsolete round.
    if (!ppgWindowValid) return false;

    const CnnEvaluation cnn = evaluateCnnAt(targetUs);
    if (cnn.inferenceRan) applyGate(cnn);

    RiskRoundResult round{};
    round.roundId = ++nextRoundId;
    round.targetUs = targetUs;
    round.cnn = cnn.result;
    round.ppg = ppgSensor.riskResult(targetUs, ppgStartUs, ppgEndUs, true);

    DepthReading depthReading{};
    if (sensorBuffers.latestDepth(targetUs, depthReading)) {
      round.depth = depthRiskEstimator.compare(depthReading);
      const uint64_t depthWindowEarliestUs = targetUs >
              static_cast<uint64_t>(Config::DEPTH_REGRESSION_WINDOW_MS) * 1000ULL +
                  Config::RISK_ROUND_MAX_DEPTH_ENDPOINT_SKEW_US
          ? targetUs - static_cast<uint64_t>(Config::DEPTH_REGRESSION_WINDOW_MS) *
                          1000ULL - Config::RISK_ROUND_MAX_DEPTH_ENDPOINT_SKEW_US
          : 0;
      const bool depthEndpointAligned = depthReading.timestampUs <= targetUs &&
          targetUs - depthReading.timestampUs <=
              Config::RISK_ROUND_MAX_DEPTH_ENDPOINT_SKEW_US &&
          depthReading.rateWindowStartUs <= depthReading.rateWindowEndUs &&
          depthReading.rateWindowStartUs >= depthWindowEarliestUs &&
          depthReading.rateWindowEndUs <= targetUs &&
          targetUs - depthReading.rateWindowEndUs <=
              Config::RISK_ROUND_MAX_DEPTH_ENDPOINT_SKEW_US;
      if (!depthEndpointAligned) {
        round.depth.valid = false;
        round.depth.rateDifferenceMps = NAN;
        round.depth.sinkRiskScore = NAN;
      }
    }

    lastRoundTargetUs = targetUs;
    logMessage(LogLevel::DEBUG, "ROUND",
        "id=%lu T=%llu CNN=%.3f/%u [%llu..%llu] depth=%.3f/%u [%llu..%llu] PPG=%.3f/%u dummy=%u [%llu..%llu]",
        static_cast<unsigned long>(round.roundId),
        static_cast<unsigned long long>(round.targetUs), round.cnn.score,
        static_cast<unsigned>(round.cnn.valid),
        static_cast<unsigned long long>(round.cnn.inputStartUs),
        static_cast<unsigned long long>(round.cnn.inputEndUs),
        round.depth.sinkRiskScore, static_cast<unsigned>(round.depth.valid),
        static_cast<unsigned long long>(round.depth.inputStartUs),
        static_cast<unsigned long long>(round.depth.inputEndUs),
        round.ppg.score, static_cast<unsigned>(round.ppg.valid),
        static_cast<unsigned>(round.ppg.usedDummy),
        static_cast<unsigned long long>(round.ppg.inputStartUs),
        static_cast<unsigned long long>(round.ppg.inputEndUs));
    return true;
  };

  uint32_t notification = 0;
  for (;;) {
    if (xTaskNotifyWait(0, UINT32_MAX, &notification, portMAX_DELAY) != pdTRUE)
      continue;

    bool ppgWindowAdvanced = false;
    uint64_t ppgWindowEndUs = 0;
    const bool ppgAnalyzeRequested =
        (notification & PROCESSING_NOTIFY_PPG_ANALYZE) != 0;
    if ((notification & (PROCESSING_NOTIFY_PPG_UPDATED |
                         PROCESSING_NOTIFY_PPG_ANALYZE)) != 0) {
      ppgWindowAdvanced = processPpgSamples(ppgAnalyzeRequested);
      if (ppgWindowAdvanced) {
        PpgAnalysis analysis{};
        const uint64_t nowUs = static_cast<uint64_t>(esp_timer_get_time());
        if (sensorBuffers.latestPpgAnalysis(nowUs, analysis))
          ppgWindowEndUs = analysis.inputEndUs;
        else
          ppgWindowAdvanced = false;
      }
    }

    bool attemptedRoundThisWake = false;
    if (!gate.isOpen() && (notification & PROCESSING_NOTIFY_IMU) != 0) {
      const uint64_t targetUs = static_cast<uint64_t>(esp_timer_get_time());
      const CnnEvaluation cnn = evaluateCnnAt(targetUs);
      const GateTransition transition = applyGate(cnn);
      if (transition == GateTransition::OPENED) {
        openingRoundPending = true;
        openingTargetUs = cnn.result.inputEndUs;
        uint64_t roundTargetUs = openingTargetUs;
        if (ppgWindowAdvanced && ppgWindowEndUs > roundTargetUs)
          roundTargetUs = ppgWindowEndUs;
        if (runRiskRound(roundTargetUs)) openingRoundPending = false;
        attemptedRoundThisWake = true;
      }
    }

    if (gate.isOpen() && ppgWindowAdvanced && !attemptedRoundThisWake) {
      uint64_t roundTargetUs = ppgWindowEndUs;
      if (openingRoundPending && openingTargetUs > roundTargetUs)
        roundTargetUs = openingTargetUs;
      if (runRiskRound(roundTargetUs)) openingRoundPending = false;
    }
  }
}
