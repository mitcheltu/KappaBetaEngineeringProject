import Foundation
import XCTest
@testable import WearableCore

final class HistoryStoreTests: XCTestCase {
    private func temporaryDirectory() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("KappaBetaTest-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    func testDurableResumeRetriesGapsAndExport() async throws {
        let directory = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let fixture = try WireFixture.load(), raw = Data(hex: fixture.recordBlockHex)
        let identity = try HistoryIdentity.decode(Data(hex: fixture.identityHex))
        let store = try HistoryStore(directory: directory)
        let initial = try await store.ensureSession(identity: identity)
        XCTAssertEqual(initial.nextSequence, 0)
        let gap = try await store.commitGap(identity: identity, first: 0, end: 100)
        XCTAssertEqual(gap.missingCount, 100)
        let committed = try await store.commitBlock(identity: identity, first: 100, bytes: raw)
        XCTAssertEqual(committed.nextSequence, 105); XCTAssertEqual(committed.recordCount, 5)
        // New instance loads only durable frames. Duplicate ACK retry does not double count.
        let reopened = try HistoryStore(directory: directory)
        let resumed = try await reopened.ensureSession(identity: identity)
        XCTAssertEqual(resumed, committed)
        let retry = try await reopened.commitBlock(identity: identity, first: 100, bytes: raw)
        XCTAssertEqual(retry, committed)
        let gapRetry = try await reopened.commitGap(identity: identity, first: 0, end: 100)
        XCTAssertEqual(gapRetry, committed)
        let page = try await reopened.readEntries(sessionKey: identity.key, afterSequence: 100, limit: 2)
        XCTAssertEqual(page.map(\.sequence), [101, 102])
        let export = try await reopened.exportCSV(sessionKey: identity.key, to: directory.appendingPathComponent("export.csv"))
        let text = try String(contentsOf: export, encoding: .utf8)
        XCTAssertTrue(text.contains("9007199254740993"))
        XCTAssertTrue(text.contains("-2000.0,2000.0,123.4"))
        XCTAssertTrue(text.contains("gap,020000123456,0102030405060708,0,100,"))
        XCTAssertEqual(text.components(separatedBy: "\r\n").count, 8) // Header, gap, five records, final newline.
    }

    func testConflictingRetryAndUnreportedGapNeverAdvanceCursor() async throws {
        let directory = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let fixture = try WireFixture.load(), identity = try HistoryIdentity.decode(Data(hex: fixture.identityHex))
        let store = try HistoryStore(directory: directory), raw = Data(hex: fixture.recordBlockHex)
        do {
            _ = try await store.commitBlock(identity: identity, first: 100, bytes: raw)
            XCTFail("Unreported gap accepted")
        } catch { }
        _ = try await store.commitGap(identity: identity, first: 0, end: 100)
        _ = try await store.commitBlock(identity: identity, first: 100, bytes: raw)
        var conflicting = raw; conflicting[16] ^= 1
        do {
            _ = try await store.commitBlock(identity: identity, first: 100, bytes: conflicting)
            XCTFail("Conflicting retry accepted")
        } catch { }
        do {
            _ = try await store.commitGap(identity: identity, first: 0, end: 99)
            XCTFail("Conflicting gap accepted")
        } catch { }
        let current = try await store.ensureSession(identity: identity)
        XCTAssertEqual(current.nextSequence, 105); XCTAssertEqual(current.recordCount, 5)
    }

    func testTornTailIsRecoveredBeforeAppending() async throws {
        let directory = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let fixture = try WireFixture.load(), identity = try HistoryIdentity.decode(Data(hex: fixture.identityHex))
        let store = try HistoryStore(directory: directory)
        _ = try await store.commitGap(identity: identity, first: 0, end: 100)
        let url = directory.appendingPathComponent("\(identity.deviceId)-\(identity.bootId).kbjournal")
        let file = try FileHandle(forWritingTo: url)
        let goodLength = try file.seekToEnd()
        try file.write(contentsOf: Data([20, 0, 0, 0, 123, 34])) // Incomplete payload, no CRC.
        try file.close()
        let recovered = try HistoryStore(directory: directory)
        let session = try await recovered.ensureSession(identity: identity)
        XCTAssertEqual(session.nextSequence, 100)
        XCTAssertEqual(try Data(contentsOf: url).count, Int(goodLength))
        let next = try await recovered.commitBlock(identity: identity, first: 100, bytes: Data(hex: fixture.recordBlockHex))
        XCTAssertEqual(next.nextSequence, 105)
    }

    func testCorruptCompleteJournalFailsClosed() async throws {
        let directory = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let fixture = try WireFixture.load(), identity = try HistoryIdentity.decode(Data(hex: fixture.identityHex))
        let store = try HistoryStore(directory: directory)
        _ = try await store.commitGap(identity: identity, first: 0, end: 100)
        let url = directory.appendingPathComponent("\(identity.deviceId)-\(identity.bootId).kbjournal")
        var bytes = try Data(contentsOf: url); bytes[bytes.count - 1] ^= 1
        try bytes.write(to: url)
        let reopened = try HistoryStore(directory: directory)
        do {
            _ = try await reopened.ensureSession(identity: identity)
            XCTFail("Corrupted journal accepted")
        } catch { }
        XCTAssertEqual(try Data(contentsOf: url), bytes)
    }

    func testIdentityCannotEscapeStorageDirectory() async throws {
        let directory = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = try HistoryStore(directory: directory)
        do {
            _ = try await store.ensureSession(identity: HistoryIdentity(deviceId: "../../escape", bootId: "0000000000000000", flags: 0))
            XCTFail("Unsafe identity accepted")
        } catch { }
    }
}
