import XCTest
import WearableCore
@testable import KappaBeta

final class HistoryTransferTests: XCTestCase {
    @MainActor
    func testCommitCompletesBeforeACKAndReconnectUsesCheckpoint() async throws {
        let harness = try TransferHarness()
        defer { harness.close() }
        await harness.transfer.start()
        try await eventually { harness.writes.count == 1 }
        let token = harness.token
        let record = sampleRecord(sequence: 0)
        // READY is optional: a lost READY must not prevent the finite transfer.
        harness.transfer.handleStatus(status(.block, token: token, first: 0, end: 1,
            detail: HistoryCRC.crc32(record), count: 1))
        harness.transfer.handleData(fragment(token: token, first: 0, payload: record))
        try await eventually { harness.writes.contains(where: { $0.first == HistoryCommand.ack.rawValue }) }
        XCTAssertEqual(harness.cursorObservedAtACK, [1])
        XCTAssertEqual(harness.commits, 1)
        harness.transfer.cancel()

        let resumed = try TransferHarness(directory: harness.directory)
        defer { resumed.close(removeDirectory: false) }
        await resumed.transfer.start()
        try await eventually { resumed.writes.count == 1 }
        XCTAssertEqual(read32(resumed.writes[0], at: 4), 1)
    }

    @MainActor
    func testLostACKRetryCannotBlockNextBlock() async throws {
        let harness = try TransferHarness()
        defer { harness.close() }
        await harness.transfer.start()
        try await eventually { !harness.writes.isEmpty }
        let token = harness.token
        let first = sampleRecord(sequence: 0), second = sampleRecord(sequence: 1)
        let oldStatus = status(.block, token: token, first: 0, end: 2, detail: HistoryCRC.crc32(first), count: 1)
        harness.transfer.handleStatus(oldStatus)
        harness.transfer.handleData(fragment(token: token, first: 0, payload: first))
        try await eventually { harness.commits == 1 }
        harness.transfer.handleStatus(oldStatus)
        harness.transfer.handleStatus(status(.block, token: token, first: 1, end: 2,
            detail: HistoryCRC.crc32(second), count: 1))
        harness.transfer.handleData(fragment(token: token, first: 0, payload: first))
        harness.transfer.handleData(fragment(token: token, first: 1, payload: second))
        try await eventually { harness.cursorObservedAtACK.last == 2 }
        XCTAssertEqual(harness.commits, 2)
        let sessions = try await harness.store.listSessions()
        XCTAssertEqual(sessions.first?.nextSequence, 2)
        XCTAssertEqual(sessions.first?.recordCount, 2)
        XCTAssertTrue(harness.errors.isEmpty)
    }

    @MainActor
    func testBadCRCAndStaleTokensNeverAcknowledgeData() async throws {
        let harness = try TransferHarness()
        defer { harness.close() }
        await harness.transfer.start()
        try await eventually { !harness.writes.isEmpty }
        let token = harness.token, record = sampleRecord(sequence: 0)
        let stale = token == UInt16.max ? 1 : token + 1
        harness.transfer.handleStatus(status(.block, token: stale, first: 0, end: 1,
            detail: HistoryCRC.crc32(record), count: 1))
        harness.transfer.handleData(fragment(token: stale, first: 0, payload: record))
        XCTAssertEqual(harness.commits, 0)
        harness.transfer.handleStatus(status(.block, token: token, first: 0, end: 1,
            detail: HistoryCRC.crc32(record) ^ 1, count: 1))
        harness.transfer.handleData(fragment(token: token, first: 0, payload: record))
        XCTAssertEqual(harness.errors.count, 1)
        XCTAssertFalse(harness.writes.contains(where: { $0.first == HistoryCommand.ack.rawValue }))
        let sessions = try await harness.store.listSessions()
        XCTAssertEqual(sessions.first?.nextSequence, 0)
    }

    @MainActor
    func testGapIsDurableBeforeACKAndLiveWaitsForConfirmation() async throws {
        let harness = try TransferHarness()
        defer { harness.close() }
        await harness.transfer.start()
        try await eventually { !harness.writes.isEmpty }
        let token = harness.token
        harness.transfer.handleStatus(status(.ready, token: token, first: 4, end: 4, detail: 64))
        harness.transfer.handleStatus(status(.gap, token: token, first: 0, end: 4))
        try await eventually { harness.cursorObservedAtACK == [4] }
        harness.transfer.handleStatus(status(.end, token: token, first: 4, end: 4))
        try await eventually { harness.writes.contains(where: { $0.first == HistoryCommand.live.rawValue }) }
        XCTAssertEqual(harness.liveCount, 0)
        harness.transfer.handleStatus(status(.live, token: token, first: 4, end: 4, detail: 64))
        XCTAssertEqual(harness.liveCount, 1)
        XCTAssertEqual(harness.progress?.missingCount, 4)
        XCTAssertEqual(harness.progress?.recordCount, 0)
        XCTAssertEqual(harness.progress?.phase, .live)
    }

    @MainActor
    func testMissingHistoryMemoryIsExplicitLiveOnly() async throws {
        let harness = try TransferHarness(flags: 0)
        defer { harness.close() }
        await harness.transfer.start()
        try await eventually { !harness.writes.isEmpty }
        let token = harness.token
        harness.transfer.handleStatus(status(.ready, token: token, first: 0, end: 0, flags: 0))
        harness.transfer.handleStatus(status(.end, token: token, first: 0, end: 0, flags: 0))
        try await eventually { harness.writes.count == 2 }
        harness.transfer.handleStatus(status(.live, token: token, first: 0, end: 0, flags: 0))
        XCTAssertEqual(harness.liveCount, 1)
        XCTAssertEqual(harness.progress?.phase, .liveOnly)
        XCTAssertEqual(harness.commits, 0)
        let sessions = try await harness.store.listSessions()
        XCTAssertTrue(sessions.isEmpty)
    }

    @MainActor
    private func eventually(_ condition: () -> Bool, file: StaticString = #filePath, line: UInt = #line) async throws {
        for _ in 0..<200 {
            if condition() { return }
            try await Task.sleep(nanoseconds: 10_000_000)
        }
        XCTFail("Timed out waiting for transfer", file: file, line: line)
        throw BluetoothFailure("Timed out waiting for transfer")
    }
}

@MainActor
private final class TransferHarness {
    let directory: URL
    let store: HistoryStore
    var transfer: HistoryTransfer!
    var writes: [Data] = []
    var cursorObservedAtACK: [UInt32] = []
    var errors: [Error] = []
    var progress: HistoryProgress?
    var liveCount = 0
    var commits = 0
    var token: UInt16 { UInt16(writes[0][2]) | UInt16(writes[0][3]) << 8 }

    init(directory: URL? = nil, flags: UInt8 = 1) throws {
        self.directory = directory ?? FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        store = try HistoryStore(directory: self.directory)
        let identity = HistoryIdentity(deviceId: "010203040506", bootId: "0708090a0b0c0d0e", flags: flags)
        transfer = HistoryTransfer(identity: identity, store: store, write: { [weak self] data in
            guard let self else { return }
            if data.first == HistoryCommand.ack.rawValue {
                let sessions = try await self.store.listSessions()
                self.cursorObservedAtACK.append(sessions.first?.nextSequence ?? UInt32.max)
            }
            self.writes.append(data)
        }, readStatus: { throw BluetoothFailure("Unexpected test status probe") },
        onProgress: { [weak self] in self?.progress = $0 },
        onCommitted: { [weak self] in self?.commits += 1 },
        onLive: { [weak self] in self?.liveCount += 1 },
        onError: { [weak self] in self?.errors.append($0) })
    }

    func close(removeDirectory: Bool = true) {
        transfer.cancel()
        if removeDirectory { try? FileManager.default.removeItem(at: directory) }
    }
}

private func status(_ type: HistoryStatus.Kind, token: UInt16, first: UInt32, end: UInt32,
                    detail: UInt32 = 0, count: UInt16 = 0, flags: UInt16 = 1) -> Data {
    var data = Data([type.rawValue, 1])
    append(token, to: &data); append(first, to: &data); append(end, to: &data)
    append(detail, to: &data); append(count, to: &data); append(flags, to: &data)
    return data
}

private func fragment(token: UInt16, first: UInt32, payload: Data) -> Data {
    var data = Data()
    append(token, to: &data); append(UInt16(0), to: &data); append(first, to: &data)
    data.append(payload)
    return data
}

private func sampleRecord(sequence: UInt32) -> Data {
    var data = Data()
    append(sequence, to: &data)
    data.append(contentsOf: [4, 1, 0, 0])
    append(UInt64(123_456), to: &data)
    data.append(Data(repeating: 0, count: 16))
    return data
}

private func append<T: FixedWidthInteger>(_ value: T, to data: inout Data) {
    for index in 0..<MemoryLayout<T>.size { data.append(UInt8(truncatingIfNeeded: value >> (index * 8))) }
}

private func read32(_ data: Data, at offset: Int) -> UInt32 {
    (0..<4).reduce(UInt32(0)) { $0 | UInt32(data[offset + $1]) << ($1 * 8) }
}
