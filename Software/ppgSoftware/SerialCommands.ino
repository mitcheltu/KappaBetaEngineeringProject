#include "FirmwareShared.h"
#include <cstdio>

namespace {
void printStatus(const AnalysisStatus& s) {
  Serial.printf("sensor=%u HR=%ld(valid=%u) SpO2=%ld(valid=%u) O=%.4f O_alarm=%u pulse=%.5f P_alarm=%u qualified=%u wearer=%lu full=%u samples=%lu drops=%lu gaps=%lu analysis_us=%lu max_us=%lu overruns=%lu\n",
      sensorReady.load(), long(s.hr), s.hrValid, long(s.spo2), s.spo2Valid,
      double(s.concern), s.oxygenActive, double(s.pulseScore), s.pulseActive, s.qualified,
      static_cast<unsigned long>(s.wearer), s.windowFull, static_cast<unsigned long>(s.samples),
      static_cast<unsigned long>(s.queueDrops), static_cast<unsigned long>(s.sensorGaps),
      static_cast<unsigned long>(s.analysisUs), static_cast<unsigned long>(s.worstAnalysisUs),
      static_cast<unsigned long>(s.missedBudgets));
}
}

void serviceSerial() {
  static AnalysisStatus latest{};
  static uint32_t lastPrint = 0;
  static char line[80] = {};
  static size_t length = 0;
  static bool overflow = false;
  if (statusQueue) xQueueReceive(statusQueue, &latest, 0);
  if (millis() - lastPrint >= 2000) { lastPrint = millis(); printStatus(latest); }
  // Bound input work so pasted text cannot monopolize the loop.
  for (unsigned i = 0; i < 80 && Serial.available(); ++i) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c != '\n') {
      if (length + 1 < sizeof(line)) line[length++] = c;
      else overflow = true;
      continue;
    }
    line[length] = '\0';
    AnalysisCommand command{};
    bool submit = false;
    if (!overflow && strcmp(line, "ack") == 0) {
      command.kind = CommandKind::Acknowledge; submit = true;
    } else if (!overflow && strcmp(line, "revoke") == 0) {
      command.kind = CommandKind::Revoke; submit = true;
    } else if (!overflow && strcmp(line, "status") == 0) {
      printStatus(latest);
    } else if (!overflow && strncmp(line, "qualify ", 8) == 0) {
      unsigned long wearer = 0, seconds = 0; char extra = 0;
      if (sscanf(line + 8, "%lu %lu %c", &wearer, &seconds, &extra) == 2 && wearer &&
          seconds && seconds <= Config::QUALIFICATION_MAX_SECONDS) {
        command = {CommandKind::Qualify, static_cast<uint32_t>(wearer), static_cast<uint32_t>(seconds)};
        submit = true;
        Serial.println("Qualification asserts independent reference verification at this placement. Requires full valid window; inspect qualified=1.");
      } else Serial.println("Use qualify <nonzero wearer-id> <seconds 1..3600> after reference verification.");
    } else if (length || overflow) {
      Serial.println("help | status | ack | revoke | qualify <wearer-id> <seconds>. Ack clears local latches; persistent conditions may alert again. No pump commands.");
    }
    if (submit) Serial.println(commandQueue && xQueueSend(commandQueue, &command, 0) == pdTRUE
        ? "Command queued; inspect status." : "Command queue unavailable/full; retry.");
    length = 0; overflow = false;
  }
}
