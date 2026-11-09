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
NimBLECharacteristic* statusCharacteristic = nullptr;

// Compatibility handshake only. No ring storage, record appends, PSRAM or ML
// allocations: the web app receives capacity=0 and selects its live-only path.
History::Buffer emptyBuffer;
History::Transfer transfer(emptyBuffer);
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
    logMessage(LogLevel::INFO, "BLE", "Connected; awaiting live-only handshake");
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
ServerCallbacks serverCallbacks;
CommandCallbacks commandCallbacks;
ZeroCallbacks zeroCallbacks;

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

  auto* identityCharacteristic = service->createCharacteristic(Config::HISTORY_INFO_UUID, NIMBLE_PROPERTY::READ, 20);
  statusCharacteristic = service->createCharacteristic(Config::HISTORY_STATUS_UUID, readable, 20);
  // Required by the web app's service discovery; no history frames are sent.
  service->createCharacteristic(Config::HISTORY_DATA_UUID, NIMBLE_PROPERTY::NOTIFY, 244);
  auto* command = service->createCharacteristic(Config::HISTORY_COMMAND_UUID, NIMBLE_PROPERTY::WRITE, 8);
  command->setCallbacks(&commandCallbacks);
  History::Identity identity{};
  esp_read_mac(identity.deviceMac, ESP_MAC_BT);
  esp_fill_random(identity.bootNonce, sizeof(identity.bootNonce));
  identityCharacteristic->setValue(reinterpret_cast<const uint8_t*>(&identity), sizeof(identity));
  History::Status ready{};
  ready.type = static_cast<uint8_t>(History::StatusType::READY);
  statusCharacteristic->setValue(reinterpret_cast<const uint8_t*>(&ready), sizeof(ready));
  publishTelemetry(millis());
  server->start();
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->enableScanResponse(true);
  advertising->addServiceUUID(Config::SERVICE_UUID);
  advertising->setName(Config::DEVICE_NAME);
  const bool started = advertising->start();
  logMessage(started ? LogLevel::INFO : LogLevel::ERROR, "BLE", "%s advertising=%d; live only, no recording",
             Config::DEVICE_NAME, started);
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
  if (peer == BLE_HS_CONN_HANDLE_NONE || epoch.load() != currentEpoch) return;
  transfer.tick(now);
  for (unsigned sent = 0; sent < 4 && epoch.load() == currentEpoch; ++sent) {
    History::Status reply{};
    if (!transfer.nextStatus(reply)) break;
    statusCharacteristic->setValue(reinterpret_cast<const uint8_t*>(&reply), sizeof(reply));
    if (!statusCharacteristic->notify(peer)) break; // Keep the exact response pending for retry.
    transfer.markStatusSent(millis());
  }
  if (epoch.load() == currentEpoch) live.store(transfer.liveEnabled());
}

void publishTelemetry(uint32_t now) {
  static uint16_t sequence = 0;
  const PacketHeader header{1, 0, sequence++, now};
  const bool pressureFresh = sensors.pressureValid && now - sensors.pressureMs < 1000;
  const bool zeroed = pressureFresh && std::isfinite(sensors.surfacePa);
  DepthPacket depth{};
  depth.header = header;
  depth.header.flags = (pressureFresh ? 1 : 0) | (zeroed ? 2 : 0);
  depth.pressurePa = pressureFresh ? sensors.pressurePa : NAN;
  depth.temperatureC = pressureFresh ? sensors.temperatureC : NAN;
  depth.depthM = zeroed ? (sensors.pressurePa - sensors.surfacePa) / (Config::FLUID_DENSITY_KG_M3 * 9.80665f) : NAN;
  ImuPacket imu{};
  imu.header = header;
  if (sensors.imuValid && now - sensors.imuMs < 300) {
    imu.header.flags = 1;
    for (size_t i = 0; i < 6; ++i)
      imu.axes[i] = lroundf(sensors.imuAxes[i] * (i < 3 ? 1000.0f : 10.0f));
  }
  PpgPacket ppg{};
  ppg.header = header;
  ppg.hrTenths = ppg.spo2Hundredths = UINT16_MAX;
  if (sensors.rawPpgValid && now - sensors.ppgMs < 300) {
    ppg.header.flags = 1;
    ppg.red = sensors.red; ppg.ir = sensors.ir;
    if (now - sensors.estimateMs < 2000) {
      if (sensors.hrValid) { ppg.header.flags |= 2; ppg.hrTenths = sensors.heartRate * 10; }
      if (sensors.spo2Valid) { ppg.header.flags |= 4; ppg.spo2Hundredths = sensors.oxygenSaturation * 100; }
    }
  }
  SystemPacket system{};
  const uint32_t controls = actuatorState.load();
  system.header = header;
  system.batteryMv = batteryMv.load();
  system.header.flags = 0x80 | (system.batteryMv != UINT16_MAX ? 1 : 0) |
      (controlReady && Config::ENABLE_ACTUATORS ? 2 : 0) | static_cast<uint8_t>(controls);
  system.countdownRemainingMs = static_cast<uint16_t>(controls >> 8);
  system.ppgOverflowCount = sensors.ppgOverflows;
  system.sensorErrors = sensors.sensorErrors;
  DemoBle::sendPacket(DemoBle::depthCharacteristic, &depth);
  DemoBle::sendPacket(DemoBle::imuCharacteristic, &imu);
  DemoBle::sendPacket(DemoBle::ppgCharacteristic, &ppg);
  DemoBle::sendPacket(DemoBle::systemCharacteristic, &system);
}
