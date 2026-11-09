import Foundation
import WearableCore

enum HistoryPhase: String {
    case opening, downloading, saving, live, catchingUp, liveOnly, error

    var label: String {
        switch self {
        case .opening: return "Opening history"
        case .downloading: return "Downloading history"
        case .saving: return "Saving history"
        case .live: return "History saved"
        case .catchingUp: return "Saving new samples"
        case .liveOnly: return "Live only — device history unavailable"
        case .error: return "History interrupted"
        }
    }
}

struct HistoryProgress {
    let phase: HistoryPhase
    let sessionKey: String
    let deviceId: String
    let bootId: String
    let cursor: UInt32
    let start: UInt32
    let target: UInt32?
    let capacity: UInt32?
    let recordCount: UInt64
    let missingCount: UInt64
    let flags: UInt16
}

/// One finite snapshot at a time. Only durable store completion permits an ACK.
/// All state is owned by the main actor; disk work runs on HistoryStore's actor.
@MainActor
final class HistoryTransfer {
    private enum Phase { case idle, opening, receiving, committing, awaitingLive }
    private let identity: HistoryIdentity
    private let store: HistoryStore
    private let write: (Data) async throws -> Void
    private let readStatus: () async throws -> Data
    private let onProgress: (HistoryProgress) -> Void
    private let onCommitted: () -> Void
    private let onLive: () -> Void
    private let onError: (Error) -> Void
    private var phase: Phase = .idle
    private var active = true
    private var live = false
    private var pollingEnabled = false
    private var token = UInt16.random(in: 1...UInt16.max)
    private var cursor: UInt32 = 0
    private var startCursor: UInt32 = 0
    private var target: UInt32?
    private var capacity: UInt32?
    private var flags: UInt16
    private var session: HistorySession?
    private var assembler: HistoryBlockAssembler?
    private var lastBlock: HistoryBlockAssembler?
    private var pendingGap: (first: UInt32, end: UInt32)?
    private var lastGap: (first: UInt32, end: UInt32)?
    private var lastProgress = ProcessInfo.processInfo.systemUptime
    private var lastProbe = ProcessInfo.processInfo.systemUptime
    private var probePending = false
    private var liveWritePending = false
    private var watchdog: Task<Void, Never>?
    private var pollTask: Task<Void, Never>?

    init(identity: HistoryIdentity, store: HistoryStore,
         write: @escaping (Data) async throws -> Void,
         readStatus: @escaping () async throws -> Data,
         onProgress: @escaping (HistoryProgress) -> Void,
         onCommitted: @escaping () -> Void,
         onLive: @escaping () -> Void,
         onError: @escaping (Error) -> Void) {
        self.identity = identity
        self.store = store
        self.write = write
        self.readStatus = readStatus
        self.onProgress = onProgress
        self.onCommitted = onCommitted
        self.onLive = onLive
        self.onError = onError
        self.flags = UInt16(identity.flags)
    }

    func start() async {
        do {
            if identity.flags & 1 != 0 { session = try await store.ensureSession(identity: identity) }
            guard active else { return }
            cursor = session?.nextSequence ?? 0
            watchdog = Task { [weak self] in
                while !Task.isCancelled {
                    do { try await Task.sleep(nanoseconds: 500_000_000) } catch { break }
                    self?.tick()
                }
            }
            begin(.open)
        } catch { fail(error) }
    }

    func cancel() {
        active = false
        watchdog?.cancel()
        watchdog = nil
        pollTask?.cancel()
        pollTask = nil
        assembler = nil
    }

    func enablePolling() {
        pollingEnabled = true
        if phase == .idle { schedulePoll() }
    }

    private func progress(_ override: HistoryPhase? = nil) {
        onProgress(HistoryProgress(phase: override ?? (live ? .catchingUp : phase == .committing ? .saving : .downloading),
            sessionKey: identity.key, deviceId: identity.deviceId, bootId: identity.bootId,
            cursor: cursor, start: startCursor, target: target, capacity: capacity,
            recordCount: session?.recordCount ?? 0, missingCount: session?.missingCount ?? 0, flags: flags))
    }

    private func fail(_ error: Error) {
        guard active else { return }
        progress(.error)
        cancel()
        // Disconnecting the transport cancels the firmware's transfer. No ACK is sent.
        onError(error)
    }

    private func send(_ command: HistoryCommand, cursor explicitCursor: UInt32? = nil) {
        guard active else { return }
        do {
            let data = try command.encode(token: token, cursor: explicitCursor ?? cursor)
            Task { [weak self] in
                guard let self, self.active else { return }
                do { try await self.write(data) } catch { self.fail(error) }
            }
        } catch { fail(error) }
    }

    private func adoptTarget(_ end: UInt32) throws {
        guard target == nil || target == end else { throw BluetoothFailure("History snapshot boundary changed during transfer.") }
        guard end >= cursor else { throw BluetoothFailure("Wearable history is behind the saved checkpoint. Saved data was retained.") }
        target = end
    }

    private func begin(_ command: HistoryCommand) {
        guard active, phase == .idle else { return }
        token = token == UInt16.max ? 1 : token + 1
        startCursor = cursor
        target = nil
        assembler = nil
        pendingGap = nil
        lastBlock = nil
        lastGap = nil
        phase = .opening
        lastProgress = ProcessInfo.processInfo.systemUptime
        lastProbe = lastProgress
        progress(live ? .catchingUp : .opening)
        send(command)
    }

    private func schedulePoll() {
        guard active, pollingEnabled, live, capacity != 0, flags & 1 != 0,
              !(flags & 4 != 0 && cursor == UInt32.max) else { return }
        pollTask?.cancel()
        pollTask = Task { [weak self] in
            do { try await Task.sleep(nanoseconds: 250_000_000) } catch { return }
            self?.begin(.poll)
        }
    }

    private func requestLive() {
        guard active, !liveWritePending else { return }
        liveWritePending = true
        do {
            let data = try HistoryCommand.live.encode(token: token, cursor: cursor)
            Task { [weak self] in
                guard let self, self.active else { return }
                defer { self.liveWritePending = false }
                do { try await self.write(data) } catch { self.fail(error) }
            }
        } catch { liveWritePending = false; fail(error) }
    }

    private func afterCommit(_ saved: HistorySession, expectedToken: UInt16, end: UInt32) {
        guard active, expectedToken == token else { return }
        guard saved.nextSequence == end else {
            fail(BluetoothFailure("Local checkpoint changed unexpectedly. Reconnect to resume.")); return
        }
        session = saved
        cursor = saved.nextSequence
        lastBlock = assembler
        lastGap = pendingGap
        assembler = nil
        pendingGap = nil
        phase = .receiving
        lastProgress = ProcessInfo.processInfo.systemUptime
        onCommitted()
        progress()
        send(.ack, cursor: end)
    }

    func handleStatus(_ data: Data) {
        guard active else { return }
        do {
            let status = try HistoryStatus.decode(data)
            guard status.token == token else { return }
            flags = status.flags
            switch status.type {
            case .ready:
                guard phase != .idle else { return }
                guard status.first <= status.end, status.count == 0 else { throw BluetoothFailure("Invalid history READY bounds.") }
                try adoptTarget(status.end)
                capacity = status.detail
                guard capacity != 0 || status.end == 0 else { throw BluetoothFailure("Invalid live-only boundary.") }
                if phase == .opening { phase = .receiving; lastProgress = ProcessInfo.processInfo.systemUptime }
                progress()
            case .block:
                guard phase != .awaitingLive, phase != .idle else { return }
                try adoptTarget(status.end)
                let end = UInt64(status.first) + UInt64(status.count)
                if end < UInt64(cursor) { return }
                if let lastBlock, status.first == lastBlock.status.first {
                    guard lastBlock.matches(status) else { throw BluetoothFailure("Retried history block changed after saving.") }
                    send(.ack, cursor: UInt32(end))
                    return
                }
                guard status.first <= cursor, status.first == cursor || end == UInt64(cursor), pendingGap == nil else {
                    throw BluetoothFailure("History block skipped the saved checkpoint.")
                }
                if let assembler {
                    guard assembler.matches(status) else { throw BluetoothFailure("History block changed before acknowledgement.") }
                } else {
                    assembler = try HistoryBlockAssembler(status: status)
                    phase = .receiving
                    lastProgress = ProcessInfo.processInfo.systemUptime
                }
                progress()
            case .gap:
                guard phase != .awaitingLive, phase != .idle, status.end >= cursor else { return }
                guard status.count == 0, status.end > status.first,
                      target == nil || status.end <= target!,
                      status.first <= cursor, status.first == cursor || status.end == cursor else {
                    throw BluetoothFailure("Invalid history gap bounds.")
                }
                if let lastGap, lastGap.first == status.first, lastGap.end == status.end {
                    send(.ack, cursor: status.end); return
                }
                guard assembler == nil else { throw BluetoothFailure("History gap arrived before a block was saved.") }
                if let pendingGap {
                    guard pendingGap.first == status.first, pendingGap.end == status.end else {
                        throw BluetoothFailure("History gap changed before acknowledgement.")
                    }
                    return
                }
                pendingGap = (status.first, status.end)
                phase = .committing
                lastProgress = ProcessInfo.processInfo.systemUptime
                progress()
                let expectedToken = token
                Task { [weak self] in
                    guard let self, self.active else { return }
                    do {
                        let saved = try await self.store.commitGap(identity: self.identity, first: status.first, end: status.end)
                        self.afterCommit(saved, expectedToken: expectedToken, end: status.end)
                    } catch { self.fail(error) }
                }
            case .end:
                guard status.first == status.end, status.end == cursor, status.count == 0,
                      phase != .committing else { throw BluetoothFailure("History ended before all data was saved.") }
                guard phase != .idle else { return }
                try adoptTarget(status.end)
                if phase != .awaitingLive { lastProgress = ProcessInfo.processInfo.systemUptime }
                phase = .awaitingLive
                requestLive()
            case .live:
                guard phase != .idle else { return }
                guard phase == .awaitingLive, status.first == cursor, status.end >= cursor, status.count == 0 else {
                    throw BluetoothFailure("Unexpected history-to-live boundary.")
                }
                capacity = status.detail
                phase = .idle
                assembler = nil
                lastProgress = ProcessInfo.processInfo.systemUptime
                let wasLive = live
                live = true
                progress(capacity == 0 || flags & 1 == 0 ? .liveOnly : .live)
                if !wasLive { onLive() }
                schedulePoll()
            case .error:
                throw BluetoothFailure(status.first == 1
                    ? "Wearable rejected the history checkpoint. Saved sessions were retained."
                    : status.first == 2 ? "History transfer timed out. Reconnect to resume."
                    : "Wearable rejected a history command. Reconnect to retry.")
            }
        } catch { fail(error) }
    }

    func handleData(_ data: Data) {
        guard active, data.count >= 8 else { return }
        let bytes = [UInt8](data)
        let packetToken = UInt16(bytes[0]) | UInt16(bytes[1]) << 8
        let first = UInt32(bytes[4]) | UInt32(bytes[5]) << 8 | UInt32(bytes[6]) << 16 | UInt32(bytes[7]) << 24
        guard packetToken == token else { return }
        do {
            if let lastBlock, first == lastBlock.status.first { _ = try lastBlock.accept(data); return }
            guard let assembler, first == assembler.status.first else { return }
            let result = try assembler.accept(data)
            if result.added > 0 { lastProgress = ProcessInfo.processInfo.systemUptime }
            guard result.complete, phase != .committing else { return }
            phase = .committing
            progress()
            let expectedToken = token
            let blockBytes = assembler.bytes
            let blockFirst = assembler.status.first
            let blockEnd = blockFirst + UInt32(assembler.status.count)
            Task { [weak self] in
                guard let self, self.active else { return }
                do {
                    let saved = try await self.store.commitBlock(identity: self.identity, first: blockFirst, bytes: blockBytes)
                    self.afterCommit(saved, expectedToken: expectedToken, end: blockEnd)
                } catch { self.fail(error) }
            }
        } catch { fail(error) }
    }

    private func tick() {
        guard active, phase != .idle else { return }
        let now = ProcessInfo.processInfo.systemUptime
        if now - lastProgress >= 20 {
            fail(BluetoothFailure("History stopped responding. Reconnect to resume saved progress.")); return
        }
        guard phase != .committing, !probePending, now - lastProbe >= 3 else { return }
        lastProbe = now
        probePending = true
        if phase == .awaitingLive { requestLive() }
        Task { [weak self] in
            guard let self, self.active else { return }
            defer { self.probePending = false }
            do {
                let data = try await self.readStatus()
                self.handleStatus(data)
            } catch { self.fail(error) }
        }
    }
}
