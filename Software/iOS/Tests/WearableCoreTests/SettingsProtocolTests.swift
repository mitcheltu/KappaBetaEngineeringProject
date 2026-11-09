import Foundation
import XCTest
@testable import WearableCore

final class SettingsProtocolTests: XCTestCase {
    private let defaults = WearableSettings(heartRate: 70, maxDepthM: 2, weightKg: 70, heightCm: 170)
    private let defaultBytes = Data([1, 1, 70, 0, 208, 7, 188, 2, 170, 0, 0, 0])

    func testSharedFirmwareVectorAndUnconfiguredRead() throws {
        XCTAssertEqual(WearableUUIDs.settings, "e81b6221-7e57-4d8e-b6f4-889c1d4e8270")
        XCTAssertEqual(try defaults.encode(), defaultBytes)
        XCTAssertEqual(try WearableSettings.decode(defaultBytes), defaults)
        XCTAssertNil(try WearableSettings.decode(Data([1] + Array(repeating: 0, count: 11))))
    }

    func testBoundsAndRoundingInWireUnits() throws {
        let minimum = WearableSettings(heartRate: 20, maxDepthM: 0.1, weightKg: 20, heightCm: 80)
        let maximum = WearableSettings(heartRate: 140, maxDepthM: 10, weightKg: 150, heightCm: 250)
        XCTAssertEqual(try WearableSettings.decode(minimum.encode()), minimum)
        XCTAssertEqual(try WearableSettings.decode(maximum.encode()), maximum)
        // 200 lb is 90.718474 kg; only the wire value is rounded to 90.7 kg.
        let converted = WearableSettings(heartRate: 70, maxDepthM: 2.0006, weightKg: 90.718474, heightCm: 170)
        let decoded = try XCTUnwrap(WearableSettings.decode(converted.encode()))
        XCTAssertEqual(decoded.maxDepthM, 2.001)
        XCTAssertEqual(decoded.weightKg, 90.7)
    }

    func testRejectsInvalidOriginalValuesBeforeRounding() {
        let invalid = [
            WearableSettings(heartRate: 70.5, maxDepthM: 2, weightKg: 70, heightCm: 170),
            WearableSettings(heartRate: .nan, maxDepthM: 2, weightKg: 70, heightCm: 170),
            WearableSettings(heartRate: 70, maxDepthM: 0.0999, weightKg: 70, heightCm: 170),
            WearableSettings(heartRate: 70, maxDepthM: 10.0001, weightKg: 70, heightCm: 170),
            WearableSettings(heartRate: 70, maxDepthM: 2, weightKg: 150.001, heightCm: 170),
            WearableSettings(heartRate: 70, maxDepthM: 2, weightKg: .infinity, heightCm: 170),
            WearableSettings(heartRate: 70, maxDepthM: 2, weightKg: 70, heightCm: 170.5)
        ]
        for settings in invalid { XCTAssertThrowsError(try settings.encode()) }
    }

    func testRejectsMalformedFirmwareResponses() {
        XCTAssertThrowsError(try WearableSettings.decode(defaultBytes.dropLast()))
        for (offset, value) in [(0, UInt8(2)), (1, 2), (1, 0), (2, 19), (10, 1)] {
            var bytes = defaultBytes
            bytes[offset] = value
            XCTAssertThrowsError(try WearableSettings.decode(bytes))
        }
    }

    @MainActor
    func testSyncWaitsForWriteAndExactReadback() async throws {
        var events: [String] = []
        let result = try await SettingsSynchronization.send(saved: defaults, supported: true,
            write: { bytes in
                XCTAssertEqual(bytes, self.defaultBytes)
                events.append("write")
                await Task.yield()
                events.append("write completed")
            }, read: {
                events.append("read")
                return self.defaultBytes
            }, isCurrent: { true })
        events.append("connected")
        XCTAssertEqual(events, ["write", "write completed", "read", "connected"])
        XCTAssertEqual(result, .synced(defaults))
    }

    @MainActor
    func testNoSavedProfileAndOldFirmwareDoNotWrite() async throws {
        for (saved, supported, expected) in [
            (Optional<WearableSettings>.none, true, SettingsSyncResult.noSavedSettings),
            (Optional(defaults), false, SettingsSyncResult.unsupported)
        ] {
            let result = try await SettingsSynchronization.send(saved: saved, supported: supported,
                write: { _ in XCTFail("Unexpected write") },
                read: { XCTFail("Unexpected read"); return Data() }, isCurrent: { true })
            XCTAssertEqual(result, expected)
        }
    }

    @MainActor
    func testReadbackMismatchAndMalformedResponseNeverReportSynced() async throws {
        var different = defaultBytes
        different[2] = 71
        for response in [different, Data(), Data([1] + Array(repeating: 0, count: 11))] {
            do {
                _ = try await SettingsSynchronization.send(saved: defaults, supported: true,
                    write: { _ in }, read: { response }, isCurrent: { true })
                XCTFail("Unconfirmed settings reported success")
            } catch { XCTAssertTrue(error is WearableError) }
        }
    }

    @MainActor
    func testWriteFailureDoesNotReadAndReadFailurePropagates() async {
        let failure = WearableError.invalidPacket("Transport failed")
        do {
            _ = try await SettingsSynchronization.send(saved: defaults, supported: true,
                write: { _ in throw failure },
                read: { XCTFail("Read after failed write"); return Data() }, isCurrent: { true })
            XCTFail("Failed write reported success")
        } catch { XCTAssertEqual(error as? WearableError, failure) }
        do {
            _ = try await SettingsSynchronization.send(saved: defaults, supported: true,
                write: { _ in }, read: { throw failure }, isCurrent: { true })
            XCTFail("Failed read reported success")
        } catch { XCTAssertEqual(error as? WearableError, failure) }
    }

    @MainActor
    func testConnectionChangeStopsReadOrDiscardsReadback() async {
        var current = true
        do {
            _ = try await SettingsSynchronization.send(saved: defaults, supported: true,
                write: { _ in current = false },
                read: { XCTFail("Read after disconnect"); return Data() }, isCurrent: { current })
            XCTFail("Stale connection reported success")
        } catch { XCTAssertTrue(error is CancellationError) }
        current = true
        do {
            _ = try await SettingsSynchronization.send(saved: defaults, supported: true,
                write: { _ in }, read: { current = false; return self.defaultBytes }, isCurrent: { current })
            XCTFail("Stale readback reported success")
        } catch { XCTAssertTrue(error is CancellationError) }
    }
}
