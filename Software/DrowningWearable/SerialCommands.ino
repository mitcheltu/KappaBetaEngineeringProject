#include "FirmwareShared.h"

void processCommand(const char* command) {
  if (strcmp(command, "z") == 0) { zeroRequested.store(true); return; }
  if (strcmp(command, "off") == 0) { stopRequested.store(true); Serial.println("Vibration/pump stop requested."); return; }
  if (strcmp(command, "sos") == 0) {
    if (!controlReady || !Config::ENABLE_ACTUATOR_TESTS) {
      Serial.println("Actuator test disabled.");
    } else if ((actuatorState.load() & (4 | 8 | 64)) != 0 ||
               batteryMv.load() == UINT16_MAX || batteryMv.load() < Config::ACTUATOR_MIN_BATTERY_MV) {
      Serial.println("SOS test rejected: already active, cancel or battery.");
    } else {
      startRequested.store(true);
      Serial.println("SOS test requested: vibration 10s, then pump running until Cancel/off.");
    }
    return;
  }
  Serial.println("Commands (newline): z = surface zero; sos = manual test; off = stop both outputs.");
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
      length = 0; overflow = false;
    } else if (length + 1 < sizeof(line) && !overflow) line[length++] = c;
    else overflow = true;
  }
}
