#include "FirmwareShared.h"
#include "HistoryBuffer.h"
#include "HistoryTransfer.h"
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <freertos/queue.h>

History::Buffer historyBuffer;
History::Transfer historyTransfer(historyBuffer);
History::Identity historyIdentity{};
void* mlWorkspaces[2] = {};
NimBLECharacteristic *historyInfoCharacteristic = nullptr, *historyStatusCharacteristic = nullptr;
NimBLECharacteristic* historyDataCharacteristic = nullptr;
std::atomic<uint16_t> historyConnection{BLE_HS_CONN_HANDLE_NONE};
std::atomic<uint32_t> historyEpoch{0};
std::atomic<bool> historyGate{false};
uint32_t historyHandledEpoch = 0;

struct QueuedHistoryCommand {
  History::Command command;
  uint32_t epoch;
  uint16_t connection;
};
StaticQueue_t historyQueueDefinition;
uint8_t historyQueueStorage[8 * sizeof(QueuedHistoryCommand)];
QueueHandle_t historyQueue = nullptr;
// Callbacks count failures; the loop reports them at a bounded rate.
std::atomic<uint32_t> historyWrongSession{0}, historyMalformedCommands{0}, historyQueueDrops{0};

class HistoryCommandCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
    const auto value = characteristic->getValue();
    QueuedHistoryCommand queued{};
    if (!historyQueue) { historyQueueDrops.fetch_add(1); return; }
    if (info.getConnHandle() != historyConnection.load()) { historyWrongSession.fetch_add(1); return; }
    if (!History::parseCommand(reinterpret_cast<const uint8_t*>(value.data()), value.size(), queued.command)) {
      historyMalformedCommands.fetch_add(1); return;
    }
    queued.epoch = historyEpoch.load();
    queued.connection = info.getConnHandle();
    if (xQueueSend(historyQueue, &queued, 0) != pdTRUE) historyQueueDrops.fetch_add(1);
  }
};
HistoryCommandCallbacks historyCommandCallbacks;

uint16_t historyFlags() {
  uint16_t flags = 0;
  if (historyBuffer.capacity()) flags |= History::ALLOCATED;
  if (mlWorkspaces[0] && mlWorkspaces[1]) flags |= History::ML_RESERVED;
  if (historyBuffer.exhausted()) flags |= History::SEQUENCE_EXHAUSTED;
  return flags;
}

void updateHistoryIdentity() {
  historyIdentity.flags = static_cast<uint8_t>(historyFlags());
  if (historyInfoCharacteristic)
    historyInfoCharacteristic->setValue(reinterpret_cast<const uint8_t*>(&historyIdentity), sizeof(historyIdentity));
}

void initializeHistory() {
  historyQueue = xQueueCreateStatic(8, sizeof(QueuedHistoryCommand), historyQueueStorage, &historyQueueDefinition);
  if (!historyQueue) logMessage(LogLevel::ERROR, "HISTORY", "Command queue allocation failed.");
  historyIdentity.version = 1;
  historyIdentity.recordSize = sizeof(History::Record);
  esp_read_mac(historyIdentity.deviceMac, ESP_MAC_BT);
  esp_fill_random(historyIdentity.bootNonce, sizeof(historyIdentity.bootNonce));
  const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  const size_t initialFree = heap_caps_get_free_size(caps);
  if (psramFound() && initialFree >= 2 * Config::ML_WORKSPACE_BYTES + Config::PSRAM_HEADROOM_BYTES) {
    for (auto& workspace : mlWorkspaces)
      workspace = heap_caps_aligned_alloc(16, Config::ML_WORKSPACE_BYTES, caps);
    if (mlWorkspaces[0] && mlWorkspaces[1]) {
      for (size_t bytes = Config::HISTORY_BYTES; bytes >= 1024 * 1024; bytes /= 2) {
        if (heap_caps_get_free_size(caps) < bytes + Config::PSRAM_HEADROOM_BYTES) continue;
        auto* storage = static_cast<History::Record*>(heap_caps_aligned_alloc(16, bytes, caps));
        if (storage) { historyBuffer.reset(storage, bytes / sizeof(History::Record)); break; }
      }
    } else {
      for (auto& workspace : mlWorkspaces) { heap_caps_free(workspace); workspace = nullptr; }
    }
  }
  updateHistoryIdentity();
  logMessage(LogLevel::INFO, "HISTORY", "%lu records / %lu bytes; ML reserve=%lu bytes; PSRAM free=%lu largest=%lu.",
      static_cast<unsigned long>(historyBuffer.capacity()),
      static_cast<unsigned long>(historyBuffer.capacity() * sizeof(History::Record)),
      static_cast<unsigned long>((historyFlags() & History::ML_RESERVED) ? 2 * Config::ML_WORKSPACE_BYTES : 0),
      static_cast<unsigned long>(heap_caps_get_free_size(caps)),
      static_cast<unsigned long>(heap_caps_get_largest_free_block(caps)));
  if (!historyBuffer.capacity())
    logMessage(LogLevel::WARN, "HISTORY", "Unavailable: PSRAM found=%u; enable OPI PSRAM and check memory. Live readings remain available.",
        static_cast<unsigned>(psramFound()));
}

void* historyMlWorkspace(size_t index) {
  return index < 2 ? mlWorkspaces[index] : nullptr;
}

void createHistoryCharacteristics(NimBLEService* service) {
  const uint32_t readable = NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY;
  historyInfoCharacteristic = service->createCharacteristic(Config::HISTORY_INFO_UUID, NIMBLE_PROPERTY::READ, 20);
  historyStatusCharacteristic = service->createCharacteristic(Config::HISTORY_STATUS_UUID, readable, 20);
  historyDataCharacteristic = service->createCharacteristic(Config::HISTORY_DATA_UUID, NIMBLE_PROPERTY::NOTIFY, 244);
  auto* command = service->createCharacteristic(Config::HISTORY_COMMAND_UUID, NIMBLE_PROPERTY::WRITE, 8);
  command->setCallbacks(&historyCommandCallbacks);
  updateHistoryIdentity();
  History::Status initial{};
  initial.type = static_cast<uint8_t>(History::StatusType::READY);
  initial.version = 1;
  initial.first = historyBuffer.oldest(); initial.end = historyBuffer.next();
  initial.detail = historyBuffer.capacity();
  initial.flags = historyFlags();
  historyStatusCharacteristic->setValue(reinterpret_cast<const uint8_t*>(&initial), sizeof(initial));
}

void historyConnected(NimBLEServer* server, NimBLEConnInfo& info) {
  uint16_t expected = BLE_HS_CONN_HANDLE_NONE;
  if (!historyConnection.compare_exchange_strong(expected, info.getConnHandle())) {
    logMessage(LogLevel::WARN, "HISTORY", "Rejecting extra BLE connection handle=%u; owner=%u.",
        static_cast<unsigned>(info.getConnHandle()), static_cast<unsigned>(expected));
    server->disconnect(info);
    return;
  }
  historyGate.store(false);
  historyEpoch.fetch_add(1);
  History::Status initial{};
  initial.type = static_cast<uint8_t>(History::StatusType::READY);
  historyStatusCharacteristic->setValue(reinterpret_cast<const uint8_t*>(&initial), sizeof(initial));
  server->updateConnParams(info.getConnHandle(), 12, 24, 0, 400);
}

void historyDisconnected(NimBLEConnInfo& info) {
  uint16_t expected = info.getConnHandle();
  if (historyConnection.compare_exchange_strong(expected, BLE_HS_CONN_HANDLE_NONE)) {
    historyGate.store(false);
    historyEpoch.fetch_add(1);
  }
}

bool historyLiveEnabled() {
  return historyGate.load() && historyEpoch.load() == historyHandledEpoch &&
         historyConnection.load() != BLE_HS_CONN_HANDLE_NONE;
}

uint16_t historyConnectionHandle() {
  return historyConnection.load();
}

void serviceHistory() {
  static uint32_t lastCommandReport = 0;
  const uint32_t now = millis();
  if (now - lastCommandReport >= 5000) {
    lastCommandReport = now;
    const uint32_t wrongSession = historyWrongSession.exchange(0);
    const uint32_t malformed = historyMalformedCommands.exchange(0);
    const uint32_t dropped = historyQueueDrops.exchange(0);
    if (wrongSession || malformed || dropped)
      logMessage(LogLevel::WARN, "HISTORY", "Commands rejected in last 5s: wrongSession=%lu malformed=%lu queueFullOrUnavailable=%lu.",
          static_cast<unsigned long>(wrongSession), static_cast<unsigned long>(malformed), static_cast<unsigned long>(dropped));
  }
  const uint32_t epoch = historyEpoch.load();
  if (epoch != historyHandledEpoch) {
    logMessage(LogLevel::DEBUG, "HISTORY", "Connection epoch changed; resetting transfer and live gate.");
    historyTransfer.disconnect();
    historyHandledEpoch = epoch;
    historyGate.store(false);
  }
  const uint16_t connection = historyConnection.load();
  historyTransfer.setFlags(historyFlags());
  if (historyIdentity.flags != historyFlags()) {
    logMessage((historyFlags() & History::SEQUENCE_EXHAUSTED) ? LogLevel::ERROR : LogLevel::INFO,
        "HISTORY", "Storage flags changed: 0x%02x -> 0x%02x (allocated=1 ML=2 sequenceExhausted=4).",
        static_cast<unsigned>(historyIdentity.flags), static_cast<unsigned>(historyFlags()));
    updateHistoryIdentity();
  }
  QueuedHistoryCommand queued{};
  for (unsigned i = 0; i < 4 && xQueueReceive(historyQueue, &queued, 0) == pdTRUE; ++i) {
    if (connection != BLE_HS_CONN_HANDLE_NONE && queued.epoch == epoch && queued.connection == connection) {
      const bool accepted = historyTransfer.handle(queued.command, millis());
      const auto opcode = static_cast<History::Opcode>(queued.command.opcode);
      const bool majorCommand = opcode == History::Opcode::OPEN || opcode == History::Opcode::ABORT;
      logMessage(accepted && majorCommand ? LogLevel::INFO : LogLevel::DEBUG, "HISTORY",
          "Command %s: opcode=%u (OPEN=1 ACK=2 LIVE=3 POLL=4 ABORT=5) token=%u cursor=%lu.",
          accepted ? "accepted" : "ignored/rejected (state/token/cursor)",
          static_cast<unsigned>(queued.command.opcode), static_cast<unsigned>(queued.command.token),
          static_cast<unsigned long>(queued.command.cursor));
    } else {
      logMessage(LogLevel::DEBUG, "HISTORY", "Discarded queued command from an old connection/session.");
    }
  }
  if (connection == BLE_HS_CONN_HANDLE_NONE || !historyStatusCharacteristic) return;
  historyTransfer.tick(millis());
  const bool live = historyTransfer.liveEnabled();
  if (historyGate.exchange(live) != live)
    logMessage(LogLevel::INFO, "HISTORY", "Live telemetry %s; cursor=%lu snapshotEnd=%lu.",
        live ? "enabled" : "paused", static_cast<unsigned long>(historyTransfer.cursor()),
        static_cast<unsigned long>(historyTransfer.snapshotEnd()));
  const uint16_t mtu = bleServer->getPeerMTU(connection);
  const size_t payloadLimit = mtu > 247 ? 244 : (mtu >= 23 ? mtu - 3 : 20);
  const auto reportNotifyFailure = [connection]() {
    static uint32_t failures = 0, lastReport = 0;
    ++failures;
    const uint32_t now = millis();
    if (lastReport == 0 || now - lastReport >= 5000) {
      logMessage(LogLevel::WARN, "HISTORY", "Notify enqueue failed: handle=%u failuresSinceReport=%lu; transfer will retry. Check subscriptions/link.",
          static_cast<unsigned>(connection), static_cast<unsigned long>(failures));
      failures = 0; lastReport = now;
    }
  };
  for (unsigned sent = 0; sent < 4 && historyEpoch.load() == epoch; ++sent) {
    History::Status status{};
    if (historyTransfer.nextStatus(status)) {
      historyStatusCharacteristic->setValue(reinterpret_cast<const uint8_t*>(&status), sizeof(status));
      if (!historyStatusCharacteristic->notify(connection)) { reportNotifyFailure(); break; }
      const auto type = static_cast<History::StatusType>(status.type);
      if (type == History::StatusType::ERROR) {
        logMessage(LogLevel::WARN, "HISTORY", "Transfer error: code=%lu (badCursor=1 timeout=2 invalidState=3) token=%u end=%lu.",
            static_cast<unsigned long>(status.first), static_cast<unsigned>(status.token), static_cast<unsigned long>(status.end));
      } else if (type == History::StatusType::GAP) {
        logMessage(LogLevel::WARN, "HISTORY", "History overwritten: missing records [%lu,%lu), token=%u.",
            static_cast<unsigned long>(status.first), static_cast<unsigned long>(status.end), static_cast<unsigned>(status.token));
      } else {
        logMessage(LogLevel::DEBUG, "HISTORY", "Status sent: type=%u (ready=1 block=2 end=3 gap=4 live=5 error=6) token=%u first=%lu end=%lu count=%u.",
            static_cast<unsigned>(status.type), static_cast<unsigned>(status.token),
            static_cast<unsigned long>(status.first), static_cast<unsigned long>(status.end), static_cast<unsigned>(status.count));
      }
      historyTransfer.markStatusSent(millis());
    } else {
      uint8_t frame[244];
      const size_t length = historyTransfer.nextFragment(frame, payloadLimit);
      if (!length) break;
      if (!historyDataCharacteristic->notify(frame, length, connection)) { reportNotifyFailure(); break; }
      historyTransfer.markFragmentSent(millis());
    }
  }
}

void recordHistory(uint8_t kind, uint8_t flags, uint64_t timeUs, const void* payload, size_t bytes) {
  historyBuffer.append(History::makeRecord(static_cast<History::Kind>(kind), flags, timeUs, payload, bytes));
}

void recordDepthSample() {
  const DepthReading& reading = depthSensor.reading();
  const bool pressurePresent = reading.sensorValid;
  const bool zeroed = pressurePresent && reading.depthValid;
  const float payload[4] = {
    pressurePresent ? reading.pressurePa : NAN,
    zeroed ? reading.filteredDepthM : NAN,
    pressurePresent ? reading.temperatureC : NAN, reading.surfacePressurePa
  };
  const uint64_t timestampUs = reading.timestampUs != 0 ? reading.timestampUs : esp_timer_get_time();
  recordHistory(1, (pressurePresent ? 1 : 0) | (zeroed ? 2 : 0), timestampUs, payload, sizeof(payload));
}

void recordImuSample() {
  int16_t payload[6] = {};
  if (imuValid)
    for (size_t i = 0; i < 6; ++i) payload[i] = lroundf(imuAxes[i] * (i < 3 ? 1000.0f : 10.0f));
  recordHistory(2, imuValid ? 1 : 0, esp_timer_get_time(), payload, sizeof(payload));
}

void recordPpgSample(uint64_t sampleTimeUs) {
  PpgPacket packet{};
  packet.red = redValue; packet.ir = irValue;
  packet.hrTenths = packet.spo2Hundredths = UINT16_MAX;
  uint8_t flags = 0x81; // Raw values present; FIFO acquisition time is estimated.
  if (millis() - estimateMs < 2000) {
    if (hrValid) { packet.hrTenths = heartRate * 10; flags |= 2; }
    if (spo2Valid) { packet.spo2Hundredths = oxygenSaturation * 100; flags |= 4; }
  }
  recordHistory(3, flags, sampleTimeUs, &packet.red, 12);
}

void recordSurfaceZero() {
  const float baseline = depthSensor.reading().surfacePressurePa;
  recordHistory(5, std::isfinite(baseline) && depthSensor.diveActive() ? 1 : 0,
                esp_timer_get_time(), &baseline, sizeof(baseline));
}

void recordPpgGap() {
  PpgPacket packet{};
  packet.hrTenths = packet.spo2Hundredths = UINT16_MAX;
  recordHistory(3, 0, esp_timer_get_time(), &packet.red, 12);
}
