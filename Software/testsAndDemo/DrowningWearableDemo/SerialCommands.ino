#include "FirmwareShared.h"

void processDemoCommand(const char* command) {
  if (strcmp(command, "status") == 0) { logStatus(); return; }
  if (strcmp(command, "off") == 0) { stopRequested.store(true); logReply("COMMAND", "Motor/pump stop requested"); return; }
  if (strcmp(command, "z") == 0) { zeroRequested.store(true); logReply("COMMAND", "Surface zero requested"); return; }
  if (strcmp(command, "log off") == 0) { setLogLevel(-1); return; }
  if (strcmp(command, "log error") == 0) { setLogLevel(0); return; }
  if (strcmp(command, "log warn") == 0) { setLogLevel(1); return; }
  if (strcmp(command, "log info") == 0) { setLogLevel(2); return; }
  if (strcmp(command, "log debug") == 0) { setLogLevel(3); return; }
  if (strcmp(command, "help") != 0) logReply("COMMAND", "Unknown command; physical SOS is the only start input");
  logReply("HELP", "status | off | z | log error/warn/info/debug/off | help (send Newline)");
  logReply("HELP", "SOS: 10s pulsed vibration, then 60s pump. Cancel/off aborts. Battery must be 3.4..4.35V");
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
