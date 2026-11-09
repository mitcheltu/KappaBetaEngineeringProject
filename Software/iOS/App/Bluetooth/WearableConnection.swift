import Foundation
import Combine
@preconcurrency import CoreBluetooth
import WearableCore

struct BluetoothFailure: LocalizedError {
    let message: String
    init(_ message: String) { self.message = message }
    var errorDescription: String? { message }
}

enum ConnectionState: String {
    case disconnected, scanning, connecting, syncing, connected, lost, error
    var label: String {
        switch self {
        case .disconnected: return "Disconnected"
        case .scanning: return "Finding wearables"
        case .connecting: return "Connecting"
        case .syncing: return "Syncing wearable"
        case .connected: return "Connected"
        case .lost: return "Connection lost"
        case .error: return "Connection error"
        }
    }
}

struct DiscoveredWearable: Identifiable {
    let id: UUID
    let name: String
    let rssi: Int
}

struct TimedTelemetry {
    let reading: TelemetryReading
    let receivedAt: Date
    let receivedUptime: TimeInterval
    var isStale = false
}

enum SettingsSyncState: Equatable {
    case waiting, syncing, noSavedSettings, unsupported
    case synced(WearableSettings)
    case failed(String)

    func description(saved: WearableSettings?) -> String {
        switch self {
        case .waiting:
            return "Saved settings will be sent on the next connection."
        case .syncing:
            return "Sending saved settings and checking the wearable's response…"
        case .noSavedSettings:
            return saved == nil ? "No saved settings; nothing was sent." : "New saved settings will be sent on the next connection."
        case .unsupported:
            return "This firmware does not support settings. Values remain saved on this iPhone."
        case .synced(let profile):
            return saved == profile ? "Saved settings confirmed by the wearable for this connection."
                : "New saved settings will be sent on the next connection."
        case .failed(let message):
            return "Settings were not confirmed: \(message)"
        }
    }
}

/// A single foreground Bluetooth session survives tab changes. It intentionally
/// disconnects on backgrounding; a new connection resumes the durable checkpoint.
@MainActor
final class WearableConnection: NSObject, ObservableObject {
    @Published private(set) var state: ConnectionState = .disconnected
    @Published private(set) var message = "Bluetooth works without Wi-Fi or mobile data."
    @Published private(set) var devices: [DiscoveredWearable] = []
    @Published private(set) var deviceName: String?
    @Published private(set) var telemetry: [TelemetryKind: TimedTelemetry] = [:]
    @Published private(set) var history: HistoryProgress?
    @Published private(set) var historyRevision = 0
    @Published private(set) var zeroPending = false
    @Published private(set) var settingsSync: SettingsSyncState = .waiting
    let store: HistoryStore
    private let savedSettings: () -> WearableSettings?

    var canConnect: Bool { [.disconnected, .lost, .error].contains(state) }
    var hasStaleReadings: Bool { telemetry.values.contains(where: \.isStale) }
    var depth: DepthReading? {
        guard case .depth(let value)? = fresh(.depth) else { return nil }; return value
    }
    var imu: IMUReading? {
        guard case .imu(let value)? = fresh(.imu) else { return nil }; return value
    }
    var ppg: PPGReading? {
        guard case .ppg(let value)? = fresh(.ppg) else { return nil }; return value
    }
    var system: SystemReading? {
        guard case .system(let value)? = fresh(.system) else { return nil }; return value
    }

    private var central: CBCentralManager?
    private var discovered: [UUID: CBPeripheral] = [:]
    private var peripheral: CBPeripheral?
    private var characteristics: [CBUUID: CBCharacteristic] = [:]
    private var scanRequested = false
    private var generation = 0
    private var transfer: HistoryTransfer?
    private var setupTask: Task<Void, Never>?
    private var deadlineTask: Task<Void, Never>?
    private var freshnessTask: Task<Void, Never>?
    private var operationTimeout: Task<Void, Never>?
    private enum OperationKind { case read, write, subscribe }
    private struct Operation {
        let id = UUID()
        let kind: OperationKind
        let characteristic: CBCharacteristic
        let bytes: Data
        let continuation: CheckedContinuation<Data, Error>
    }
    private var operations: [Operation] = []
    private var pending: Operation?

    init(store: HistoryStore, savedSettings: @escaping () -> WearableSettings? = { nil }) {
        self.store = store
        self.savedSettings = savedSettings
        super.init()
        // Constructing CBCentralManager is deferred until the user taps Find.
    }

    private func fresh(_ kind: TelemetryKind) -> TelemetryReading? {
        guard state == .connected, let value = telemetry[kind], !value.isStale,
              ProcessInfo.processInfo.systemUptime - value.receivedUptime < 3 else { return nil }
        return value.reading
    }

    func startScanning() {
        guard canConnect else { return }
        release()
        devices = []
        discovered = [:]
        history = nil
        deviceName = nil
        message = "Keep the wearable nearby and disconnect its Web App session first."
        state = .scanning
        scanRequested = true
        if central == nil {
            central = CBCentralManager(delegate: self, queue: .main,
                options: [CBCentralManagerOptionShowPowerAlertKey: true])
        } else if central?.state == .poweredOn {
            scan()
        } else {
            updateAvailability()
        }
    }

    private func scan() {
        guard scanRequested, let central, central.state == .poweredOn else { return }
        central.scanForPeripherals(withServices: [CBUUID(string: WearableUUIDs.service)],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
        let expectedGeneration = generation
        deadlineTask?.cancel()
        deadlineTask = Task { [weak self] in
            do { try await Task.sleep(nanoseconds: 15_000_000_000) } catch { return }
            guard let self, self.generation == expectedGeneration, self.state == .scanning else { return }
            self.central?.stopScan()
            self.scanRequested = false
            self.state = .disconnected
            self.message = self.devices.isEmpty
                ? "No wearable found. Check device power, move closer, and disconnect any other client."
                : "Select a wearable below, or scan again."
        }
    }

    func connect(to id: UUID) {
        guard state == .scanning || canConnect, let chosen = discovered[id], let central else { return }
        guard central.state == .poweredOn else { updateAvailability(); return }
        guard chosen.state == .disconnected else {
            message = "The previous connection is still closing. Try again shortly."; return
        }
        release()
        history = nil
        peripheral = chosen
        chosen.delegate = self
        deviceName = devices.first(where: { $0.id == id })?.name ?? chosen.name ?? "KappaBetaWearable"
        state = .connecting
        message = "Connecting over Bluetooth…"
        let expectedGeneration = generation
        central.connect(chosen, options: nil)
        deadlineTask = Task { [weak self] in
            do { try await Task.sleep(nanoseconds: 20_000_000_000) } catch { return }
            guard let self, self.generation == expectedGeneration, self.state == .connecting else { return }
            self.fail(BluetoothFailure("Connection timed out. Disconnect the Web App and try again nearby."))
        }
    }

    func disconnect() {
        release()
        state = .disconnected
        deviceName = nil
        message = "Disconnected. Saved sessions remain available offline."
    }

    func suspend() {
        guard [.scanning, .connecting, .syncing, .connected].contains(state) else { return }
        disconnect()
        message = "Bluetooth paused while the app is in the background. Reconnect to resume saved progress."
    }

    func zeroDepth() {
        guard state == .connected, !zeroPending else { return }
        zeroPending = true
        telemetry.removeValue(forKey: .depth)
        message = "Requesting a surface pressure baseline…"
        let expectedGeneration = generation
        Task { [weak self] in
            guard let self else { return }
            do {
                _ = try await self.operation(.write, uuid: WearableUUIDs.zeroDepth, bytes: Data([1]))
                guard self.generation == expectedGeneration else { return }
                self.zeroPending = false
                self.message = "Surface zero requested. Check subsequent depth readings and firmware diagnostics; a successful write does not confirm calibration."
            } catch {
                guard self.generation == expectedGeneration else { return }
                self.zeroPending = false
                self.message = error.localizedDescription
            }
        }
    }

    private func release() {
        generation += 1
        scanRequested = false
        central?.stopScan()
        deadlineTask?.cancel(); deadlineTask = nil
        setupTask?.cancel(); setupTask = nil
        freshnessTask?.cancel(); freshnessTask = nil
        operationTimeout?.cancel(); operationTimeout = nil
        transfer?.cancel(); transfer = nil
        let cancellation = BluetoothFailure("Bluetooth connection ended.")
        let abandoned = pending
        pending = nil
        let abandonedQueue = operations
        operations = []
        abandoned?.continuation.resume(throwing: cancellation)
        for operation in abandonedQueue { operation.continuation.resume(throwing: cancellation) }
        let previous = peripheral
        peripheral = nil
        previous?.delegate = nil
        if let previous, previous.state != .disconnected { central?.cancelPeripheralConnection(previous) }
        characteristics = [:]
        telemetry = [:]
        zeroPending = false
        settingsSync = .waiting
    }

    private func fail(_ error: Error) {
        let settingsWereSyncing = settingsSync == .syncing
        release()
        if settingsWereSyncing { settingsSync = .failed(error.localizedDescription) }
        state = .error
        message = error.localizedDescription
    }

    private func updateAvailability() {
        guard let central else { return }
        switch central.state {
        case .poweredOn:
            if scanRequested { scan() }
        case .unknown:
            message = "Waiting for Bluetooth…"
        case .resetting:
            fail(BluetoothFailure("Bluetooth is resetting. Wait a moment, then reconnect to resume saved progress."))
        case .poweredOff:
            fail(BluetoothFailure("Turn on Bluetooth in Settings, then find your wearable again. Wi-Fi and mobile data can remain off."))
        case .unauthorized:
            fail(BluetoothFailure("Allow Bluetooth access for Kappa Beta in iPhone Settings, then try again."))
        case .unsupported:
            fail(BluetoothFailure("Bluetooth Low Energy is unavailable on this device. Use a physical iPhone for wearable connections."))
        @unknown default:
            fail(BluetoothFailure("Bluetooth is unavailable. Try again from Settings."))
        }
    }

    private func configure() {
        let expectedGeneration = generation
        setupTask = Task { [weak self] in
            guard let self, self.generation == expectedGeneration,
                  self.state == .connecting, !Task.isCancelled else { return }
            do {
                let identityData = try await self.operation(.read, uuid: WearableUUIDs.historyIdentity)
                let identity = try HistoryIdentity.decode(identityData)
                _ = try await self.operation(.subscribe, uuid: WearableUUIDs.historyStatus)
                _ = try await self.operation(.subscribe, uuid: WearableUUIDs.historyData)
                guard self.generation == expectedGeneration else { return }
                self.state = .syncing
                self.deadlineTask?.cancel()
                self.message = "Saving the device's history before starting live readings…"
                let transfer = HistoryTransfer(identity: identity, store: self.store,
                    write: { [weak self] bytes in
                        guard let self, self.generation == expectedGeneration else { throw CancellationError() }
                        _ = try await self.operation(.write, uuid: WearableUUIDs.historyCommand, bytes: bytes)
                    }, readStatus: { [weak self] in
                        guard let self, self.generation == expectedGeneration else { throw CancellationError() }
                        return try await self.operation(.read, uuid: WearableUUIDs.historyStatus)
                    }, onProgress: { [weak self] progress in
                        guard let self, self.generation == expectedGeneration else { return }
                        self.history = progress
                    }, onCommitted: { [weak self] in
                        guard let self, self.generation == expectedGeneration else { return }
                        self.historyRevision += 1
                    }, onLive: { [weak self] in
                        guard let self, self.generation == expectedGeneration else { return }
                        self.enableLive(generation: expectedGeneration)
                    }, onError: { [weak self] error in
                        guard let self, self.generation == expectedGeneration else { return }
                        self.fail(error)
                    })
                self.transfer = transfer
                await transfer.start()
            } catch {
                guard self.generation == expectedGeneration else { return }
                self.fail(error)
            }
        }
    }

    private func enableLive(generation expectedGeneration: Int) {
        guard generation == expectedGeneration, state == .syncing else { return }
        setupTask = Task { [weak self] in
            guard let self, self.generation == expectedGeneration,
                  self.state == .syncing, !Task.isCancelled else { return }
            do {
                self.settingsSync = .syncing
                self.message = "Checking saved settings before starting live readings…"
                let result: SettingsSyncResult
                do {
                    // onLive confirms the initial history snapshot is complete. No
                    // background polls are enabled until this write/readback finishes.
                    result = try await SettingsSynchronization.send(saved: self.savedSettings(),
                        supported: self.characteristics[CBUUID(string: WearableUUIDs.settings)] != nil,
                        write: { bytes in
                            _ = try await self.operation(.write, uuid: WearableUUIDs.settings, bytes: bytes)
                        }, read: {
                            try await self.operation(.read, uuid: WearableUUIDs.settings)
                        }, isCurrent: { self.generation == expectedGeneration })
                } catch {
                    guard self.generation == expectedGeneration else { return }
                    self.fail(error)
                    self.settingsSync = .failed(error.localizedDescription)
                    return
                }
                guard self.generation == expectedGeneration else { return }
                switch result {
                case .noSavedSettings: self.settingsSync = .noSavedSettings
                case .unsupported: self.settingsSync = .unsupported
                case .synced(let profile): self.settingsSync = .synced(profile)
                }
                for kind in TelemetryKind.allCases {
                    _ = try await self.operation(.subscribe, uuid: kind.uuid)
                    let packet = try await self.operation(.read, uuid: kind.uuid)
                    guard self.generation == expectedGeneration else { return }
                    self.receive(packet, kind: kind)
                }
                self.state = .connected
                self.message = self.history?.phase == .liveOnly
                    ? "Connected. This wearable has no history memory; live readings only."
                    : "Connected. New samples are being saved on this iPhone."
                self.transfer?.enablePolling()
                self.freshnessTask = Task { [weak self] in
                    while !Task.isCancelled {
                        do { try await Task.sleep(nanoseconds: 500_000_000) } catch { break }
                        guard let self, self.generation == expectedGeneration else { return }
                        self.refreshStaleness()
                    }
                }
            } catch {
                guard self.generation == expectedGeneration else { return }
                self.fail(error)
            }
        }
    }

    private func receive(_ data: Data, kind: TelemetryKind) {
        do {
            let reading = try TelemetryPacket.decode(data, kind: kind)
            if let previous = telemetry[kind]?.reading {
                let newHeader = header(reading), oldHeader = header(previous)
                let elapsed = newHeader.uptime &- oldHeader.uptime
                guard !(elapsed == 0 && newHeader.sequence == oldHeader.sequence), elapsed < 0x8000_0000 else { return }
            }
            telemetry[kind] = TimedTelemetry(reading: reading, receivedAt: Date(), receivedUptime: ProcessInfo.processInfo.systemUptime)
        } catch {
            telemetry.removeValue(forKey: kind)
            message = error.localizedDescription
        }
    }

    private func header(_ reading: TelemetryReading) -> (sequence: UInt16, uptime: UInt32) {
        switch reading {
        case .depth(let value): return (value.sequence, value.uptimeMs)
        case .imu(let value): return (value.sequence, value.uptimeMs)
        case .ppg(let value): return (value.sequence, value.uptimeMs)
        case .system(let value): return (value.sequence, value.uptimeMs)
        }
    }

    private func refreshStaleness() {
        let now = ProcessInfo.processInfo.systemUptime
        for kind in TelemetryKind.allCases {
            if var value = telemetry[kind], !value.isStale, now - value.receivedUptime >= 3 {
                value.isStale = true
                telemetry[kind] = value
            }
        }
    }

    private func operation(_ kind: OperationKind, uuid: String, bytes: Data = Data()) async throws -> Data {
        try Task.checkCancellation()
        guard let peripheral, peripheral.state == .connected,
              let characteristic = characteristics[CBUUID(string: uuid)] else {
            throw BluetoothFailure("The wearable's Bluetooth connection is no longer available.")
        }
        return try await withCheckedThrowingContinuation { continuation in
            operations.append(Operation(kind: kind, characteristic: characteristic, bytes: bytes, continuation: continuation))
            pumpQueue()
        }
    }

    private func pumpQueue() {
        guard pending == nil, !operations.isEmpty, let peripheral else { return }
        let operation = operations.removeFirst()
        pending = operation
        switch operation.kind {
        case .read: peripheral.readValue(for: operation.characteristic)
        case .write: peripheral.writeValue(operation.bytes, for: operation.characteristic, type: .withResponse)
        case .subscribe: peripheral.setNotifyValue(true, for: operation.characteristic)
        }
        operationTimeout?.cancel()
        operationTimeout = Task { [weak self] in
            do { try await Task.sleep(nanoseconds: 10_000_000_000) } catch { return }
            guard let self, self.pending?.id == operation.id else { return }
            self.fail(BluetoothFailure("Bluetooth operation timed out. Reconnect to resume saved history."))
        }
    }

    @discardableResult
    private func finish(_ kind: OperationKind, characteristic: CBCharacteristic, data: Data = Data(), error: Error?) -> Bool {
        guard let operation = pending, operation.kind == kind, operation.characteristic === characteristic else { return false }
        pending = nil
        operationTimeout?.cancel(); operationTimeout = nil
        if let error { operation.continuation.resume(throwing: error) }
        else { operation.continuation.resume(returning: data) }
        pumpQueue()
        return true
    }
}

// CoreBluetooth delivers these callbacks on the main queue passed at creation.
// Xcode 16's preconcurrency conformance preserves that runtime isolation contract.
extension WearableConnection: @preconcurrency CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) { updateAvailability() }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard state == .scanning else { return }
        discovered[peripheral.identifier] = peripheral
        let device = DiscoveredWearable(id: peripheral.identifier,
            name: advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? peripheral.name ?? "KappaBetaWearable", rssi: RSSI.intValue)
        devices.removeAll(where: { $0.id == device.id })
        devices.append(device)
        devices.sort { $0.rssi > $1.rssi }
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        guard self.peripheral === peripheral, state == .connecting else { return }
        peripheral.discoverServices([CBUUID(string: WearableUUIDs.service)])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        guard self.peripheral === peripheral else { return }
        fail(error ?? BluetoothFailure("Could not connect. Disconnect the other client and try again."))
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        guard self.peripheral === peripheral else { return }
        release()
        state = .lost
        message = "Wearable disconnected. Reconnect to resume saved progress.\(error.map { " " + $0.localizedDescription } ?? "")"
    }
}

extension WearableConnection: @preconcurrency CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard self.peripheral === peripheral else { return }
        if let error { fail(error); return }
        guard let service = peripheral.services?.first(where: { $0.uuid == CBUUID(string: WearableUUIDs.service) }) else {
            fail(BluetoothFailure("The wearable service is missing. Check the firmware version.")); return
        }
        let required = TelemetryKind.allCases.map(\.uuid) + [WearableUUIDs.zeroDepth, WearableUUIDs.historyIdentity,
            WearableUUIDs.historyStatus, WearableUUIDs.historyData, WearableUUIDs.historyCommand]
        // Settings are optional so older firmware and the demo can still connect.
        peripheral.discoverCharacteristics((required + [WearableUUIDs.settings]).map { CBUUID(string: $0) }, for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard self.peripheral === peripheral else { return }
        if let error { fail(error); return }
        for characteristic in service.characteristics ?? [] { characteristics[characteristic.uuid] = characteristic }
        var required: [(String, CBCharacteristicProperties)] = TelemetryKind.allCases.map { ($0.uuid, [.read, .notify]) }
        required += [(WearableUUIDs.zeroDepth, .write), (WearableUUIDs.historyIdentity, .read),
            (WearableUUIDs.historyStatus, [.read, .notify]), (WearableUUIDs.historyData, .notify), (WearableUUIDs.historyCommand, .write)]
        guard required.allSatisfy({ uuid, properties in
            guard let characteristic = characteristics[CBUUID(string: uuid)] else { return false }
            return characteristic.properties.contains(properties)
        }) else {
            fail(BluetoothFailure("Firmware update required: wearable telemetry, calibration, or history characteristics are missing.")); return
        }
        if let settings = characteristics[CBUUID(string: WearableUUIDs.settings)],
           !settings.properties.contains([.read, .write]) {
            let error = BluetoothFailure("Firmware settings characteristic must support read and write.")
            fail(error)
            settingsSync = .failed(error.localizedDescription)
            return
        }
        configure()
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard self.peripheral === peripheral else { return }
        if let error {
            if !finish(.read, characteristic: characteristic, error: error) { fail(error) }
            return
        }
        guard let value = characteristic.value else {
            let error = BluetoothFailure("Wearable returned an empty Bluetooth value.")
            if !finish(.read, characteristic: characteristic, error: error) { fail(error) }
            return
        }
        // A read and a notification can race. Resolve the queued read once, but
        // dispatch status immediately so following fragments cannot outrun BLOCK.
        // The read caller may deliver this value again; handlers are idempotent.
        finish(.read, characteristic: characteristic, data: value, error: nil)
        if characteristic.uuid == CBUUID(string: WearableUUIDs.historyStatus) { transfer?.handleStatus(value) }
        else if characteristic.uuid == CBUUID(string: WearableUUIDs.historyData) { transfer?.handleData(value) }
        else if let kind = TelemetryKind.allCases.first(where: { CBUUID(string: $0.uuid) == characteristic.uuid }) { receive(value, kind: kind) }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        guard self.peripheral === peripheral else { return }
        finish(.write, characteristic: characteristic, error: error)
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        guard self.peripheral === peripheral else { return }
        let failure = error ?? (characteristic.isNotifying ? nil : BluetoothFailure("Wearable notifications could not be enabled."))
        finish(.subscribe, characteristic: characteristic, error: failure)
    }
}
