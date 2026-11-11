#include "FirmwareShared.h"

void processCommand(const char* command) {
  if (strcmp(command, "status") == 0) { logStatus(); return; }
  if (strcmp(command, "log off") == 0) { setLogLevel(-1); return; }
  if (strcmp(command, "log error") == 0) { setLogLevel(0); return; }
  if (strcmp(command, "log warn") == 0) { setLogLevel(1); return; }
  if (strcmp(command, "log info") == 0) { setLogLevel(2); return; }
  if (strcmp(command, "log debug") == 0) { setLogLevel(3); return; }
  if (strcmp(command, "z") == 0) { zeroRequested.store(true); logReply("COMMAND", "surface zero requested"); return; }
  if (strcmp(command, "calibrate gyro") == 0) {
    if (calibrateImuGyro())
      logReply("IMU", "gyro calibration queued; keep the board motionless");
    else
      logReply("IMU", "gyro calibration not queued; sensor unavailable or request already pending");
    return;
  }
  if (strcmp(command, "clear calibration") == 0) {
    clearImuCalibration();
    logReply("IMU", "clear calibration queued");
    return;
  }
  if (strcmp(command, "off") == 0) { stopRequested.store(true); logReply("COMMAND", "blue LED/pump stop requested"); return; }
  if (strcmp(command, "sos") == 0) {
    const uint32_t controls = actuatorState.load();
    const uint32_t sample = batteryDiagnostic.load();
    const BatteryCondition condition = batteryCondition(sample & 0xffff);
    const char* reason = !controlReady ? "control task unavailable" :
        !Config::ENABLE_ACTUATOR_TESTS ? "actuator tests disabled" :
        (controls & 8) ? "Cancel is held" : (controls & (4 | 64)) ? "sequence already active" :
        condition != BatteryCondition::READY ? batteryConditionName(condition) : nullptr;
    if (reason) {
      logReply("SOS", "serial request REJECTED: %s; estimated battery=%u mV; type status for details",
          reason, static_cast<unsigned>(sample & 0xffff));
    } else {
      startRequested.store(true);
      logReply("SOS", "serial request queued; control task will check interlocks and SOS rearm");
    }
    return;
  }
  if (strcmp(command, "help") != 0) logReply("COMMAND", "unknown command (type help)");
  logReply("HELP", "newline commands: status | sos | off | z | calibrate gyro | clear calibration | log error/warn/info/debug/off | help");
  logReply("HELP", "SOS needs battery 3400..4350 mV and released Cancel; release/repress SOS after an abort");
}

void serviceSerial() {
  static char line[48];
  static size_t length = 0;
  static bool overflow = false;
  // Bound processing so a continuous stream cannot starve sensor servicing.
  for (int i = 0; i < 64 && Serial.available(); ++i) {
    const char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line[length] = '\0';
      if (!overflow && length > 0) processCommand(line);
      if (overflow) logReply("COMMAND", "line too long (max 47 characters); discarded, no command executed");
      length = 0; overflow = false;
    } else if (length + 1 < sizeof(line) && !overflow) line[length++] = c;
    else overflow = true;
  }
}
