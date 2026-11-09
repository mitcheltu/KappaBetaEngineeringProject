import Foundation

/// Personal values accepted by firmware. They do not enable automatic activation.
public struct WearableSettings: Equatable, Sendable {
    public let heartRate: Double
    public let maxDepthM: Double
    public let weightKg: Double
    public let heightCm: Double

    public init(heartRate: Double, maxDepthM: Double, weightKg: Double, heightCm: Double) {
        self.heartRate = heartRate
        self.maxDepthM = maxDepthM
        self.weightKg = weightKg
        self.heightCm = heightCm
    }

    public func encode() throws -> Data {
        guard heartRate.isFinite, (20...140).contains(heartRate), heartRate.rounded() == heartRate,
              maxDepthM.isFinite, (0.1...10).contains(maxDepthM),
              weightKg.isFinite, (20...150).contains(weightKg),
              heightCm.isFinite, (80...250).contains(heightCm), heightCm.rounded() == heightCm else {
            throw WearableError.invalidPacket("Saved settings are outside the supported ranges.")
        }
        var bytes: [UInt8] = [1, 1]
        for value in [UInt16(heartRate), UInt16((maxDepthM * 1000).rounded()),
                      UInt16((weightKg * 10).rounded()), UInt16(heightCm), 0] {
            bytes.append(UInt8(truncatingIfNeeded: value))
            bytes.append(UInt8(truncatingIfNeeded: value >> 8))
        }
        return Data(bytes)
    }

    /// A fresh boot reads as version 1, configured 0, with all value bytes zero.
    public static func decode(_ data: Data) throws -> WearableSettings? {
        let bytes = Array(data)
        guard bytes.count == 12, bytes[0] == 1, bytes[1] <= 1,
              bytes[10] == 0, bytes[11] == 0 else {
            throw WearableError.invalidPacket("Unsupported or malformed wearable settings response.")
        }
        if bytes[1] == 0 {
            guard bytes.dropFirst(2).allSatisfy({ $0 == 0 }) else {
                throw WearableError.invalidPacket("Unconfigured wearable settings contain unexpected values.")
            }
            return nil
        }
        func value(_ offset: Int) -> Double {
            Double(UInt16(bytes[offset]) | (UInt16(bytes[offset + 1]) << 8))
        }
        let result = WearableSettings(heartRate: value(2), maxDepthM: value(4) / 1000,
                                      weightKg: value(6) / 10, heightCm: value(8))
        _ = try result.encode()
        return result
    }
}

public enum SettingsSyncResult: Equatable, Sendable {
    case noSavedSettings
    case unsupported
    case synced(WearableSettings)
}

/// Run after initial history completion, before enabling connected state or polling.
@MainActor
public enum SettingsSynchronization {
    public static func send(saved: WearableSettings?, supported: Bool,
                            write: (Data) async throws -> Void,
                            read: () async throws -> Data,
                            isCurrent: () -> Bool) async throws -> SettingsSyncResult {
        func checkCurrent() throws {
            try Task.checkCancellation()
            guard isCurrent() else { throw CancellationError() }
        }
        try checkCurrent()
        guard supported else { return .unsupported }
        guard let saved else { return .noSavedSettings }
        let expected = try saved.encode()
        try await write(expected)
        try checkCurrent()
        let accepted = try await read()
        try checkCurrent()
        _ = try WearableSettings.decode(accepted)
        guard accepted == expected else {
            throw WearableError.invalidPacket("The wearable did not confirm the saved settings. Reconnect to retry.")
        }
        return .synced(saved)
    }
}
