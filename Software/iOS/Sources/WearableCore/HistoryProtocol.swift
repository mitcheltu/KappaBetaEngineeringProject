import Foundation

public struct HistoryIdentity: Codable, Sendable, Equatable {
    public let deviceId: String
    public let bootId: String
    public let flags: UInt8
    public var key: String { "\(deviceId):\(bootId)" }
    public var hasHistory: Bool { flags & 1 != 0 }

    public init(deviceId: String, bootId: String, flags: UInt8) {
        self.deviceId = deviceId; self.bootId = bootId; self.flags = flags
    }
    public static func decode(_ data: Data) throws -> Self {
        let b = Bytes(data)
        guard b.count == 20, b[0] == 1, b[1] == 32, b[3] == 0, b.u16(18) == 0 else {
            throw WearableError.invalidPacket("Unsupported history identity. Update the wearable firmware.")
        }
        func hex(_ range: Range<Int>) -> String { range.map { String(format: "%02x", b[$0]) }.joined() }
        return Self(deviceId: hex(4..<10), bootId: hex(10..<18), flags: b[2])
    }
}

public struct HistoryStatus: Sendable, Equatable {
    public enum Kind: UInt8, Sendable { case ready = 1, block, end, gap, live, error }
    public let type: Kind
    public let token: UInt16
    public let first: UInt32
    public let end: UInt32
    public let detail: UInt32
    public let count: UInt16
    public let flags: UInt16

    public init(type: Kind, token: UInt16, first: UInt32, end: UInt32, detail: UInt32 = 0, count: UInt16 = 0, flags: UInt16 = 0) {
        self.type = type; self.token = token; self.first = first; self.end = end
        self.detail = detail; self.count = count; self.flags = flags
    }
    public static func decode(_ data: Data) throws -> Self {
        let b = Bytes(data)
        guard b.count == 20, b[1] == 1, let kind = Kind(rawValue: b[0]) else {
            throw WearableError.invalidPacket("Unsupported history status packet.")
        }
        return Self(type: kind, token: b.u16(2), first: b.u32(4), end: b.u32(8),
                    detail: b.u32(12), count: b.u16(16), flags: b.u16(18))
    }
}

public enum HistoryCommand: UInt8, Sendable {
    case open = 1, ack, live, poll, abort
    public func encode(token: UInt16, cursor: UInt32) throws -> Data {
        guard token != 0 else { throw WearableError.invalidPacket("History token must be nonzero.") }
        var data = Data([rawValue, 1]); data.appendLE(token); data.appendLE(cursor)
        return data
    }
}

public enum HistoryCRC {
    public static func crc32(_ data: Data) -> UInt32 {
        var crc: UInt32 = .max
        for byte in data {
            crc ^= UInt32(byte)
            for _ in 0..<8 { crc = (crc >> 1) ^ ((crc & 1) == 1 ? 0xedb88320 : 0) }
        }
        return crc ^ .max
    }
}

public struct HistoryRecord: Sendable, Equatable {
    public let sequence: UInt32
    public let kind: UInt8
    public let flags: UInt8
    public let timeUs: UInt64
    public let raw: Data

    public static func decodeBlock(_ data: Data, first: UInt32) throws -> [Self] {
        guard !data.isEmpty, data.count % 32 == 0, data.count <= 64 * 32,
              UInt64(first) + UInt64(data.count / 32) <= UInt64(UInt32.max) else {
            throw WearableError.invalidPacket("Invalid history block length or sequence bounds.")
        }
        let data = Data(data)
        return try stride(from: 0, to: data.count, by: 32).map { offset in
            let raw = data.subdata(in: offset..<(offset + 32)), b = Bytes(raw)
            guard b.u32(0) == first + UInt32(offset / 32), (1...5).contains(b[4]), b.u16(6) == 0 else {
                throw WearableError.invalidPacket("Nonconsecutive or unsupported history record.")
            }
            return Self(sequence: b.u32(0), kind: b[4], flags: b[5], timeUs: b.u64(8), raw: raw)
        }
    }
}

/// One immutable firmware block; duplicate fragments must agree byte for byte.
/// Keep this object confined to the same actor as the transfer controller.
public final class HistoryBlockAssembler {
    public let status: HistoryStatus
    private var storage: [UInt8]
    private var present: [Bool]
    private var received = 0
    public var bytes: Data { Data(storage) }

    public init(status: HistoryStatus) throws {
        let end = UInt64(status.first) + UInt64(status.count)
        guard status.type == .block, (1...64).contains(status.count),
              end <= UInt64(status.end), end <= UInt64(UInt32.max) else {
            throw WearableError.invalidPacket("Invalid history block bounds.")
        }
        self.status = status
        storage = [UInt8](repeating: 0, count: Int(status.count) * 32)
        present = [Bool](repeating: false, count: storage.count)
    }
    public func matches(_ value: HistoryStatus) -> Bool {
        status.token == value.token && status.first == value.first && status.end == value.end &&
        status.detail == value.detail && status.count == value.count && value.type == .block
    }
    public func accept(_ fragment: Data) throws -> (complete: Bool, added: Int) {
        let b = Bytes(fragment)
        guard b.count >= 9 else { throw WearableError.invalidPacket("Truncated history fragment.") }
        guard b.u16(0) == status.token, b.u32(4) == status.first else { return (received == storage.count, 0) }
        let offset = Int(b.u16(2)), payloadCount = b.count - 8
        guard offset + payloadCount <= storage.count else { throw WearableError.invalidPacket("History fragment exceeds block.") }
        var added = 0
        for i in 0..<payloadCount {
            let target = offset + i, value = b[8 + i]
            if present[target] && storage[target] != value { throw WearableError.invalidPacket("Conflicting history retry.") }
            if !present[target] { present[target] = true; received += 1; added += 1 }
            storage[target] = value
        }
        let complete = received == storage.count
        if complete && HistoryCRC.crc32(bytes) != status.detail {
            throw WearableError.invalidPacket("History checksum mismatch. Reconnect to retry.")
        }
        return (complete, added)
    }
}
