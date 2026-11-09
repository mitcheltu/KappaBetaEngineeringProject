import Foundation

public enum WearableError: Error, LocalizedError, Equatable {
    case invalidPacket(String)
    case storage(String)

    public var errorDescription: String? {
        switch self {
        case .invalidPacket(let message), .storage(let message): return message
        }
    }
}

public enum WearableUUIDs {
    public static let service = uuid("6200")
    public static let zeroDepth = uuid("6220")
    public static let settings = uuid("6221")
    public static let historyIdentity = uuid("6230")
    public static let historyStatus = uuid("6231")
    public static let historyData = uuid("6232")
    public static let historyCommand = uuid("6233")
    static func uuid(_ value: String) -> String { "e81b\(value)-7e57-4d8e-b6f4-889c1d4e8270" }
}

public enum TelemetryKind: String, CaseIterable, Sendable {
    case depth, imu, ppg, system
    public var uuid: String {
        switch self {
        case .depth: return WearableUUIDs.uuid("6210")
        case .imu: return WearableUUIDs.uuid("6211")
        case .ppg: return WearableUUIDs.uuid("6212")
        case .system: return WearableUUIDs.uuid("6213")
        }
    }
}

public struct DepthReading: Sendable, Equatable {
    public let sequence: UInt16
    public let uptimeMs: UInt32
    public let pressurePa: Double?
    public let depthM: Double?
    public let temperatureC: Double?
}
public struct IMUReading: Sendable, Equatable {
    public let sequence: UInt16
    public let uptimeMs: UInt32
    public let accelerationG: [Double]?
    public let angularVelocityDps: [Double]?
}
public struct PPGReading: Sendable, Equatable {
    public let sequence: UInt16
    public let uptimeMs: UInt32
    public let red: UInt32?
    public let infrared: UInt32?
    public let heartRateBpm: Double?
    public let spo2Percent: Double?
}
public struct SystemReading: Sendable, Equatable {
    public let sequence: UInt16
    public let uptimeMs: UInt32
    public let batteryV: Double?
    public let pumpEnabled: Bool
    public let pumpOn: Bool
    public let cancelPressed: Bool
    public let sosPressed: Bool
    public let remainingMs: UInt16
    public let vibrationOn: Bool
    public let countdownActive: Bool
    public let vibrationMotor: Bool
    public let ppgOverflowCount: UInt32
    public let sensorErrors: UInt32
}
public enum TelemetryReading: Sendable, Equatable {
    case depth(DepthReading), imu(IMUReading), ppg(PPGReading), system(SystemReading)
}

/// Fixed little-endian firmware packets. Invalid or unknown values stay nil.
public enum TelemetryPacket {
    public static func decode(_ data: Data, kind: TelemetryKind) throws -> TelemetryReading {
        let b = Bytes(data)
        guard b.count == 20, b[0] == 1 else {
            throw WearableError.invalidPacket("Unsupported \(kind.rawValue) packet length or version.")
        }
        let flags = b[1], sequence = b.u16(2), uptime = b.u32(4)
        func valid(_ mask: UInt8) -> Bool { flags & mask == mask }
        func float(_ offset: Int, _ isValid: Bool) -> Double? {
            let value = Double(b.f32(offset))
            return isValid && value.isFinite ? value : nil
        }
        switch kind {
        case .depth:
            let pressure = float(8, valid(1))
            return .depth(DepthReading(sequence: sequence, uptimeMs: uptime,
                pressurePa: pressure.flatMap { $0 > 0 ? $0 : nil },
                depthM: float(12, valid(3)), temperatureC: float(16, valid(1))))
        case .imu:
            return .imu(IMUReading(sequence: sequence, uptimeMs: uptime,
                accelerationG: valid(1) ? [8, 10, 12].map { Double(b.i16($0)) / 1000 } : nil,
                angularVelocityDps: valid(1) ? [14, 16, 18].map { Double(b.i16($0)) / 10 } : nil))
        case .ppg:
            let hr = b.u16(16), spo2 = b.u16(18)
            return .ppg(PPGReading(sequence: sequence, uptimeMs: uptime,
                red: valid(1) ? b.u32(8) : nil, infrared: valid(1) ? b.u32(12) : nil,
                heartRateBpm: valid(2) && hr > 0 && hr != .max ? Double(hr) / 10 : nil,
                spo2Percent: valid(4) && spo2 <= 10000 ? Double(spo2) / 100 : nil))
        case .system:
            let battery = b.u16(8)
            return .system(SystemReading(sequence: sequence, uptimeMs: uptime,
                batteryV: valid(1) && battery != .max ? Double(battery) / 1000 : nil,
                pumpEnabled: valid(2), pumpOn: valid(4), cancelPressed: valid(8), sosPressed: valid(16),
                remainingMs: b.u16(10), vibrationOn: valid(32), countdownActive: valid(64),
                vibrationMotor: valid(128), ppgOverflowCount: b.u32(12), sensorErrors: b.u32(16)))
        }
    }
}

/// Copies Data to avoid alignment assumptions and nonzero Data slice indices.
struct Bytes {
    let bytes: [UInt8]
    init(_ data: Data) { bytes = Array(data) }
    var count: Int { bytes.count }
    subscript(_ index: Int) -> UInt8 { bytes[index] }
    func u16(_ i: Int) -> UInt16 { UInt16(bytes[i]) | UInt16(bytes[i + 1]) << 8 }
    func i16(_ i: Int) -> Int16 { Int16(bitPattern: u16(i)) }
    func u32(_ i: Int) -> UInt32 { UInt32(u16(i)) | UInt32(u16(i + 2)) << 16 }
    func u64(_ i: Int) -> UInt64 { UInt64(u32(i)) | UInt64(u32(i + 4)) << 32 }
    func f32(_ i: Int) -> Float { Float(bitPattern: u32(i)) }
}

extension Data {
    mutating func appendLE<T: FixedWidthInteger>(_ value: T) {
        var little = value.littleEndian
        Swift.withUnsafeBytes(of: &little) { append(contentsOf: $0) }
    }
}
