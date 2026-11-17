#include "FirmwareShared.h"
#include "TelemetryProtocol.h"
#include "HistoryTransfer.h"
#include <NimBLEDevice.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <freertos/queue.h>

namespace DemoBle {
NimBLEServer* server = nullptr;
NimBLECharacteristic *depthCharacteristic = nullptr, *imuCharacteristic = nullptr;
NimBLECharacteristic *ppgCharacteristic = nullptr, *systemCharacteristic = nullptr;
NimBLECharacteristic *statusCharacteristic = nullptr, *dataCharacteristic = nullptr;
NimBLECharacteristic* identityCharacteristic = nullptr;
NimBLECharacteristic *scenarioCatalogCharacteristic = nullptr, *scenarioStatusCharacteristic = nullptr;
History::Identity identity{};

History::Transfer transfer(historyBuffer);
std::atomic<uint16_t> connection{BLE_HS_CONN_HANDLE_NONE};
std::atomic<uint32_t> epoch{0}, droppedCommands{0};
std::atomic<bool> live{false};
uint32_t handledEpoch = 0;
struct QueuedCommand {
  History::Command command;
  uint32_t epoch;
  uint16_t connection;
};
StaticQueue_t queueDefinition;
uint8_t queueStorage[8 * sizeof(QueuedCommand)];
QueueHandle_t queue = nullptr;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* owner, NimBLEConnInfo& info) override {
    uint16_t expected = BLE_HS_CONN_HANDLE_NONE;
    if (!connection.compare_exchange_strong(expected, info.getConnHandle())) {
      owner->disconnect(info);
      return;
    }
    live.store(false);
    epoch.fetch_add(1);
    owner->updateConnParams(info.getConnHandle(), 12, 24, 0, 400);
    logMessage(LogLevel::INFO, "BLE", "Connected; awaiting history download and LIVE handshake");
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) override {
    uint16_t expected = info.getConnHandle();
    if (connection.compare_exchange_strong(expected, BLE_HS_CONN_HANDLE_NONE)) {
      live.store(false);
      epoch.fetch_add(1);
      logMessage(LogLevel::INFO, "BLE", "Disconnected reason=%d; advertising again", reason);
    }
    NimBLEDevice::startAdvertising();
  }
};

class CommandCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
    const auto bytes = characteristic->getValue();
    QueuedCommand command{};
    if (!queue || info.getConnHandle() != connection.load() ||
        !History::parseCommand(bytes.data(), bytes.size(), command.command)) {
      droppedCommands.fetch_add(1);
      return;
    }
    command.epoch = epoch.load();
    command.connection = info.getConnHandle();
    if (xQueueSend(queue, &command, 0) != pdTRUE) droppedCommands.fetch_add(1);
  }
};

class ZeroCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
    const auto bytes = characteristic->getValue();
    if (info.getConnHandle() == connection.load() && bytes.size() == 1 &&
        static_cast<uint8_t>(bytes[0]) == 1) {
      zeroRequested.store(true);
      logMessage(LogLevel::INFO, "BLE", "Surface-zero requested");
    }
  }
};
// Demo scenario START/ABORT (ScenarioProtocol.h). The app can only arm a compiled-in
// scenario or abort one before actuation; it cannot start, stop or time an output.
class ScenarioCommandCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
    const auto bytes = characteristic->getValue();
    Scenario::Command command{};
    if (info.getConnHandle() != connection.load() ||
        !Scenario::parseCommand(bytes.data(), bytes.size(), command) ||
        !queueScenarioCommand(command, "App")) droppedCommands.fetch_add(1);
  }
};
ServerCallbacks serverCallbacks;
CommandCallbacks commandCallbacks;
ZeroCallbacks zeroCallbacks;
ScenarioCommandCallbacks scenarioCommandCallbacks;

void sendPacket(NimBLECharacteristic* characteristic, const void* packet) {
  if (!characteristic) return;
  characteristic->setValue(static_cast<const uint8_t*>(packet), 20);
  const uint16_t peer = connection.load();
  if (peer != BLE_HS_CONN_HANDLE_NONE && bleLiveEnabled() && !characteristic->notify(peer)) {
    static uint32_t lastReport = 0;
    const uint32_t now = millis();
    if (now - lastReport >= 5000) {
      lastReport = now;
      logMessage(LogLevel::WARN, "BLE", "Live notification queue full/unsubscribed; latest reading skipped");
    }
  }
}
} // namespace DemoBle

bool bleConnected() {
  return DemoBle::connection.load() != BLE_HS_CONN_HANDLE_NONE;
}

bool bleLiveEnabled() {
  return DemoBle::live.load() && DemoBle::epoch.load() == DemoBle::handledEpoch && bleConnected();
}

void initializeBle() {
  using namespace DemoBle;
  queue = xQueueCreateStatic(8, sizeof(QueuedCommand), queueStorage, &queueDefinition);
  if (!queue) {
    logMessage(LogLevel::ERROR, "BLE", "Command queue unavailable; Bluetooth not started");
    return;
  }
  NimBLEDevice::init(Config::DEVICE_NAME);
  NimBLEDevice::setMTU(247);
  server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCallbacks, false);
  NimBLEService* service = server->createService(Config::SERVICE_UUID);
  const uint32_t readable = NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY;
  depthCharacteristic = service->createCharacteristic(Config::DEPTH_UUID, readable, 20);
  imuCharacteristic = service->createCharacteristic(Config::IMU_UUID, readable, 20);
  ppgCharacteristic = service->createCharacteristic(Config::PPG_UUID, readable, 20);
  systemCharacteristic = service->createCharacteristic(Config::SYSTEM_UUID, readable, 20);
  auto* zero = service->createCharacteristic(Config::ZERO_DEPTH_UUID, NIMBLE_PROPERTY::WRITE, 1);
  zero->setCallbacks(&zeroCallbacks);

  identityCharacteristic = service->createCharacteristic(Config::HISTORY_INFO_UUID, NIMBLE_PROPERTY::READ, 20);
  statusCharacteristic = service->createCharacteristic(Config::HISTORY_STATUS_UUID, readable, 20);
  dataCharacteristic = service->createCharacteristic(Config::HISTORY_DATA_UUID, NIMBLE_PROPERTY::NOTIFY, 244);
  auto* command = service->createCharacteristic(Config::HISTORY_COMMAND_UUID, NIMBLE_PROPERTY::WRITE, 8);
  command->setCallbacks(&commandCallbacks);
  if (Config::DEMO_SCENARIOS_ENABLED) {
    static uint8_t catalog[sizeof(Scenario::CatalogHeader) + 8 * sizeof(Scenario::CatalogEntry)];
    const size_t catalogBytes = scenarioCatalog(catalog, sizeof(catalog));
    scenarioCatalogCharacteristic = service->createCharacteristic(
        Config::SCENARIO_CATALOG_UUID, NIMBLE_PROPERTY::READ, sizeof(catalog));
    scenarioCatalogCharacteristic->setValue(catalog, catalogBytes);
    auto* scenarioCommand = service->createCharacteristic(Config::SCENARIO_COMMAND_UUID, NIMBLE_PROPERTY::WRITE,
                                                          sizeof(Scenario::Command));
    scenarioCommand->setCallbacks(&scenarioCommandCallbacks);
    scenarioStatusCharacteristic = service->createCharacteristic(Config::SCENARIO_STATUS_UUID, readable,
                                                                 sizeof(Scenario::Status));
    Scenario::Status status{};
    scenarioStatus(status);
    scenarioStatusCharacteristic->setValue(reinterpret_cast<const uint8_t*>(&status), sizeof(status));
  }
  identity.flags = static_cast<uint8_t>(historyFlags());
  esp_read_mac(identity.deviceMac, ESP_MAC_BT);
  esp_fill_random(identity.bootNonce, sizeof(identity.bootNonce));
  identityCharacteristic->setValue(reinterpret_cast<const uint8_t*>(&identity), sizeof(identity));
  History::Status ready{};
  ready.type = static_cast<uint8_t>(History::StatusType::READY);
  ready.first = historyBuffer.oldest();
  ready.end = historyBuffer.next();
  ready.detail = historyBuffer.capacity();
  ready.flags = historyFlags();
  statusCharacteristic->setValue(reinterpret_cast<const uint8_t*>(&ready), sizeof(ready));
  publishTelemetry(millis());
  server->start();
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->enableScanResponse(true);
  advertising->addServiceUUID(Config::SERVICE_UUID);
  advertising->setName(Config::DEVICE_NAME);
  const bool started = advertising->start();
  logMessage(started ? LogLevel::INFO : LogLevel::ERROR, "BLE", "%s advertising=%d; history capacity=%lu",
             Config::DEVICE_NAME, started, static_cast<unsigned long>(historyBuffer.capacity()));
}

void serviceBle() {
  using namespace DemoBle;
  if (!queue || !statusCharacteristic) return;
  const uint32_t currentEpoch = epoch.load();
  if (currentEpoch != handledEpoch) {
    transfer.disconnect();
    live.store(false);
    handledEpoch = currentEpoch;
  }
  const uint16_t peer = connection.load();
  transfer.setFlags(historyFlags());
  if (identity.flags != historyFlags()) {
    identity.flags = static_cast<uint8_t>(historyFlags());
    identityCharacteristic->setValue(reinterpret_cast<const uint8_t*>(&identity), sizeof(identity));
  }
  QueuedCommand command{};
  for (unsigned i = 0; i < 4 && xQueueReceive(queue, &command, 0) == pdTRUE; ++i) {
    if (peer != BLE_HS_CONN_HANDLE_NONE && command.epoch == currentEpoch && command.connection == peer)
      transfer.handle(command.command, millis());
  }
  static uint32_t lastCommandReport = 0;
  const uint32_t now = millis();
  if (now - lastCommandReport >= 5000) {
    lastCommandReport = now;
    const uint32_t dropped = droppedCommands.exchange(0);
    if (dropped) logMessage(LogLevel::WARN, "BLE", "Rejected/dropped commands=%lu", static_cast<unsigned long>(dropped));
  }
  // Scenario status: notify on any change and at least once a second while connected.
  if (scenarioStatusCharacteristic) {
    static Scenario::Status lastSent{};
    static uint32_t lastSentMs = 0;
    Scenario::Status status{};
    if (scenarioStatus(status)) {
      const bool changed = memcmp(&status, &lastSent, sizeof(status)) != 0;
      scenarioStatusCharacteristic->setValue(reinterpret_cast<const uint8_t*>(&status), sizeof(status));
      if (peer != BLE_HS_CONN_HANDLE_NONE && (changed || now - lastSentMs >= 1000) &&
          scenarioStatusCharacteristic->notify(peer)) {
        lastSent = status;
        lastSentMs = now;
      }
    }
  }
  if (peer == BLE_HS_CONN_HANDLE_NONE || epoch.load() != currentEpoch) return;
  transfer.tick(now);
  const uint16_t mtu = server->getPeerMTU(peer);
  const size_t payloadLimit = mtu > 247 ? 244 : (mtu >= 23 ? mtu - 3 : 20);
  // Bounded work per pass leaves time to drain the stock PPG FIFO.
  for (unsigned sent = 0; sent < 4 && epoch.load() == currentEpoch; ++sent) {
    History::Status reply{};
    if (transfer.nextStatus(reply)) {
      statusCharacteristic->setValue(reinterpret_cast<const uint8_t*>(&reply), sizeof(reply));
      if (!statusCharacteristic->notify(peer)) break; // Keep the response pending for retry.
      transfer.markStatusSent(millis());
    } else {
      uint8_t frame[244];
      const size_t length = transfer.nextFragment(frame, payloadLimit);
      if (!length) break;
      if (!dataCharacteristic->notify(frame, length, peer)) break;
      transfer.markFragmentSent(millis());
    }
  }
  if (epoch.load() == currentEpoch) live.store(transfer.liveEnabled());
}

void publishTelemetry(uint32_t now) {
  static uint16_t sequence = 0;
  auto depth = SensorTelemetry::depth(sensors, now);
  auto imu = SensorTelemetry::imu(sensors, now);
  auto ppg = SensorTelemetry::ppg(sensors, now);
  auto system = SensorTelemetry::system(sensors, batteryMv.load(), actuatorState.load(),
                                        controlReady && Config::ENABLE_ACTUATORS && pumpOutputReady.load());
  for (auto* header : {&depth.header, &imu.header, &ppg.header, &system.header}) {
    header->sequence = sequence;
    header->uptimeMs = now;
  }
  ++sequence;
  recordSystemSample(system);
  DemoBle::sendPacket(DemoBle::depthCharacteristic, &depth);
  DemoBle::sendPacket(DemoBle::imuCharacteristic, &imu);
  DemoBle::sendPacket(DemoBle::ppgCharacteristic, &ppg);
  DemoBle::sendPacket(DemoBle::systemCharacteristic, &system);
}
