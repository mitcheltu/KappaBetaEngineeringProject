#include "FirmwareShared.h"
#include <esp_random.h>

void processDemoCommand(const char* command) {
  if (strcmp(command, "status") == 0) { logStatus(); return; }
  if (strcmp(command, "ppg") == 0) { logPpgDiagnostics(); return; }
  if (strcmp(command, "off") == 0) { stopRequested.store(true); logReply("COMMAND", "Motor/pump stop requested"); return; }
  if (strcmp(command, "z") == 0) { zeroRequested.store(true); logReply("COMMAND", "Surface zero requested"); return; }
  if (strcmp(command, "log off") == 0) { setLogLevel(-1); return; }
  if (strcmp(command, "log error") == 0) { setLogLevel(0); return; }
  if (strcmp(command, "log warn") == 0) { setLogLevel(1); return; }
  if (strcmp(command, "log info") == 0) { setLogLevel(2); return; }
  if (strcmp(command, "log debug") == 0) { setLogLevel(3); return; }
  if (strcmp(command, "scenario") == 0 || strcmp(command, "scenario list") == 0 ||
      strcmp(command, "scenario status") == 0) { logScenarioCatalog(); return; }
  if (strncmp(command, "scenario ", 9) == 0) {
    Scenario::Command c{};
    c.token = static_cast<uint16_t>(esp_random() | 1u);
    if (strcmp(command + 9, "abort") == 0) {
      c.opcode = static_cast<uint8_t>(Scenario::Opcode::ABORT);
    } else {
      char* end = nullptr;
      const long id = strtol(command + 9, &end, 10);
      if (!end || *end != '\0' || id < 1 || id > 255) { logReply("SCENARIO", "Usage: scenario | scenario <id> | scenario abort"); return; }
      c.opcode = static_cast<uint8_t>(Scenario::Opcode::START);
      c.scenarioId = static_cast<uint8_t>(id);
      c.catalogCrc = scenarioCatalogCrc();
    }
    if (!queueScenarioCommand(c, "Serial")) logReply("SCENARIO", "Command queue full/unavailable");
    return;
  }
  if (strcmp(command, "help") != 0) logReply("COMMAND", "Unknown command");
  logReply("HELP", "status | ppg | off | z | scenario | scenario <id> | scenario abort | log error/warn/info/debug/off | help");
  logReply("HELP", "SOS: 10s pulsed vibration, then 60s pump including 2s ramp. Cancel/double tap/off aborts. Battery must be 3.4..4.35V");
  logReply("HELP", "scenario <id>: 30 s to get in the water, then scripted playback through the on-device models");
}

void serviceSerial() {
  static char line[48];
  static size_t length = 0;
  static bool overflow = false;
  for (unsigned i = 0; i < 64 && Serial.available(); ++i) {
    const char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line[length] = '\0';
      if (!overflow && length) processDemoCommand(line);
      if (overflow) logReply("COMMAND", "Line too long; discarded");
      length = 0; overflow = false;
    } else if (length + 1 < sizeof(line) && !overflow) line[length++] = c;
    else overflow = true;
  }
}
