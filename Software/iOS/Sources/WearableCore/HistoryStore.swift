import Foundation

public struct HistorySession: Sendable, Equatable, Identifiable {
    public let identity: HistoryIdentity
    public var key: String { identity.key }
    public var id: String { key }
    public var deviceId: String { identity.deviceId }
    public var bootId: String { identity.bootId }
    public var flags: UInt8 { identity.flags }
    public internal(set) var nextSequence: UInt32
    public internal(set) var recordCount: UInt64
    public internal(set) var missingCount: UInt64
    public let createdAt: Date
    public internal(set) var updatedAt: Date
    public internal(set) var lastTimeUs: UInt64?
}

public struct HistoryEntry: Sendable {
    public let sequence: UInt32
    public let end: UInt32
    public let importedAt: Date
    public let record: HistoryRecord?
    public var isGap: Bool { record == nil }
}

/// A serialized, checksummed append journal. Each frame contains both data and
/// its exclusive checkpoint; a separate cursor can never get ahead of records.
/// Commit returns only after FileHandle.synchronize succeeds. A torn final frame
/// is removed on reopening. Fully written but invalid frames fail closed.
/// This improves crash recovery but cannot guarantee survival of hardware loss.
/// Create one store instance per directory; this actor is the exclusive writer.
public actor HistoryStore {
    private struct Batch: Codable, Sendable {
        let version: UInt8
        let identity: HistoryIdentity
        let first: UInt32
        let end: UInt32
        let importedAt: Date
        let bytes: Data?
        var isHeader: Bool { first == 0 && end == 0 && bytes == nil }
    }
    private struct Index: Sendable {
        let first: UInt32
        let end: UInt32
        let offset: UInt64
    }
    private struct State: Sendable {
        var session: HistorySession
        var index: [Index]
    }
    private let directory: URL
    private var cache: [String: State] = [:]
    private let encoder = JSONEncoder()
    private let maxFrameBytes = 8192

    public init(directory: URL) throws {
        self.directory = directory
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        var excluded = directory
        var values = URLResourceValues(); values.isExcludedFromBackup = true
        try? excluded.setResourceValues(values)
        #if os(iOS)
        try FileManager.default.setAttributes([.protectionKey: FileProtectionType.completeUntilFirstUserAuthentication], ofItemAtPath: directory.path)
        #endif
    }

    public func ensureSession(identity: HistoryIdentity) throws -> HistorySession {
        try load(identity: identity).session
    }

    public func commitBlock(identity: HistoryIdentity, first: UInt32, bytes: Data) throws -> HistorySession {
        let records = try HistoryRecord.decodeBlock(bytes, first: first)
        let end = first + UInt32(records.count)
        var state = try load(identity: identity)
        if first != state.session.nextSequence {
            guard end <= state.session.nextSequence else { throw storage("History cursor mismatch; refusing an unreported gap.") }
            // Compare records, including retries whose block boundaries differ.
            let saved = try entries(state: state, afterSequence: Int64(first) - 1, limit: records.count)
            guard saved.count == records.count, zip(saved, records).allSatisfy({ $0.0.record?.raw == $0.1.raw }) else {
                throw storage("Retried history conflicts with saved records.")
            }
            return state.session
        }
        let batch = Batch(version: 1, identity: identity, first: first, end: end, importedAt: Date(), bytes: bytes)
        let offset = try append(batch, to: fileURL(identity))
        state.index.append(Index(first: first, end: end, offset: offset))
        apply(batch, records: records, to: &state.session)
        cache[identity.key] = state
        return state.session
    }

    public func commitGap(identity: HistoryIdentity, first: UInt32, end: UInt32) throws -> HistorySession {
        guard end > first else { throw storage("Invalid history gap.") }
        var state = try load(identity: identity)
        if first != state.session.nextSequence {
            let saved = try entries(state: state, afterSequence: Int64(first) - 1, limit: 1).first
            guard end <= state.session.nextSequence, saved?.isGap == true,
                  saved?.sequence == first, saved?.end == end else {
                throw storage("Retried history gap conflicts with saved data.")
            }
            return state.session
        }
        let batch = Batch(version: 1, identity: identity, first: first, end: end, importedAt: Date(), bytes: nil)
        let offset = try append(batch, to: fileURL(identity))
        state.index.append(Index(first: first, end: end, offset: offset))
        apply(batch, records: nil, to: &state.session)
        cache[identity.key] = state
        return state.session
    }

    public func listSessions() throws -> [HistorySession] {
        let files = try FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)
        var sessions: [HistorySession] = []
        for url in files where url.pathExtension == "kbjournal" {
            let handle = try FileHandle(forReadingFrom: url)
            let header: Batch?
            do { header = try Self.readFrame(handle); try handle.close() }
            catch { try? handle.close(); throw error }
            guard let header else { continue } // Empty interrupted first creation has no saved session.
            guard header.isHeader, try fileURL(header.identity).lastPathComponent == url.lastPathComponent else {
                throw storage("Invalid history journal identity.")
            }
            sessions.append(try load(identity: header.identity).session)
        }
        return sessions.sorted { $0.updatedAt > $1.updatedAt }
    }

    public func readEntries(sessionKey: String, afterSequence: Int64 = -1, limit: Int = 256) throws -> [HistoryEntry] {
        guard (1...1024).contains(limit) else { throw storage("History page size must be 1–1024.") }
        return try entries(state: state(for: sessionKey), afterSequence: afterSequence, limit: limit)
    }

    /// Streams one journal frame (at most 64 sensor records) at a time.
    /// Returns a shareable, complete file; incomplete exports are removed.
    public func exportCSV(sessionKey: String, to destination: URL) async throws -> URL {
        let state = try state(for: sessionKey)
        let source = try fileURL(state.session.identity)
        // Only immutable, already committed frame offsets cross into this task.
        // Slow exports do not block incoming commits and their Bluetooth ACKs.
        return try await Task.detached(priority: .utility) {
            try Self.writeCSV(state: state, source: source, destination: destination)
        }.value
    }

    private nonisolated static func writeCSV(state: State, source: URL, destination: URL) throws -> URL {
        guard destination.standardizedFileURL != source.standardizedFileURL else {
            throw WearableError.storage("Export cannot overwrite sensor history.")
        }
        guard !FileManager.default.fileExists(atPath: destination.path) else { throw WearableError.storage("Choose a new export file name.") }
        guard FileManager.default.createFile(atPath: destination.path, contents: nil) else { throw WearableError.storage("Cannot create export file.") }
        let output = try FileHandle(forWritingTo: destination)
        do {
            try output.write(contentsOf: Data((HistoryCSV.columns.joined(separator: ",") + "\r\n").utf8))
            let input = try FileHandle(forReadingFrom: source)
            defer { try? input.close() }
            for index in state.index {
                try input.seek(toOffset: index.offset)
                guard let batch = try readFrame(input) else { throw WearableError.storage("History ended during export.") }
                for entry in try makeEntries(batch) {
                    try output.write(contentsOf: Data(try HistoryCSV.row(session: state.session, entry: entry).utf8))
                }
            }
            try output.synchronize(); try output.close()
            return destination
        } catch {
            try? output.close()
            try? FileManager.default.removeItem(at: destination)
            throw error
        }
    }

    private func state(for key: String) throws -> State {
        if let state = cache[key] { return state }
        let parts = key.split(separator: ":", omittingEmptySubsequences: false)
        guard parts.count == 2 else { throw storage("Invalid session key.") }
        let identity = HistoryIdentity(deviceId: String(parts[0]), bootId: String(parts[1]), flags: 0)
        guard FileManager.default.fileExists(atPath: try fileURL(identity).path) else { throw storage("Saved session not found.") }
        return try load(identity: identity)
    }

    private func fileURL(_ identity: HistoryIdentity) throws -> URL {
        let allowed = Set("0123456789abcdef")
        guard identity.deviceId.count == 12, identity.bootId.count == 16,
              identity.deviceId.allSatisfy({ allowed.contains($0) }), identity.bootId.allSatisfy({ allowed.contains($0) }) else {
            throw storage("Invalid device or boot-session identity.")
        }
        return directory.appendingPathComponent("\(identity.deviceId)-\(identity.bootId).kbjournal")
    }

    private func load(identity: HistoryIdentity) throws -> State {
        if let state = cache[identity.key] { return state }
        let url = try fileURL(identity)
        if !FileManager.default.fileExists(atPath: url.path) {
            guard FileManager.default.createFile(atPath: url.path, contents: nil) else { throw storage("Cannot create local history.") }
        }
        #if os(iOS)
        try FileManager.default.setAttributes([.protectionKey: FileProtectionType.completeUntilFirstUserAuthentication], ofItemAtPath: url.path)
        #endif
        let input = try FileHandle(forUpdating: url)
        defer { try? input.close() }
        var state: State?
        var lastGoodOffset: UInt64 = 0
        while let batch = try Self.readFrame(input) {
            guard batch.identity.key == identity.key, batch.version == 1 else { throw storage("History journal identity/version mismatch.") }
            if state == nil {
                guard batch.isHeader else { throw storage("History journal header is missing.") }
                state = State(session: HistorySession(identity: batch.identity, nextSequence: 0,
                    recordCount: 0, missingCount: 0, createdAt: batch.importedAt,
                    updatedAt: batch.importedAt, lastTimeUs: nil), index: [])
            } else {
                guard let current = state, batch.first == current.session.nextSequence,
                      batch.end > batch.first else { throw storage("History journal has a missing or duplicate range.") }
                let records = try batch.bytes.map { try HistoryRecord.decodeBlock($0, first: batch.first) }
                if let records, UInt64(batch.first) + UInt64(records.count) != UInt64(batch.end) {
                    throw storage("History journal checkpoint mismatch.")
                }
                state!.index.append(Index(first: batch.first, end: batch.end, offset: lastGoodOffset))
                apply(batch, records: records, to: &state!.session)
            }
            lastGoodOffset = try input.offset()
        }
        // readFrame returns nil at EOF or a partial final write, never at corrupt complete data.
        let length = try input.seekToEnd()
        if length != lastGoodOffset {
            try input.truncate(atOffset: lastGoodOffset)
        }
        // A prior failed synchronize may have left a complete frame in the OS
        // cache even if rollback also failed. Re-establish durability before
        // exposing any recovered checkpoint to the transfer controller.
        try input.synchronize()
        if state == nil {
            let date = Date()
            let header = Batch(version: 1, identity: identity, first: 0, end: 0, importedAt: date, bytes: nil)
            _ = try append(header, to: url)
            state = State(session: HistorySession(identity: identity, nextSequence: 0,
                recordCount: 0, missingCount: 0, createdAt: date, updatedAt: date, lastTimeUs: nil), index: [])
        }
        cache[identity.key] = state!
        return state!
    }

    private func append(_ batch: Batch, to url: URL) throws -> UInt64 {
        let payload = try encoder.encode(batch)
        guard payload.count <= maxFrameBytes else { throw storage("History transaction is too large.") }
        var frame = Data(); frame.appendLE(UInt32(payload.count)); frame.append(payload)
        frame.appendLE(HistoryCRC.crc32(payload))
        let file = try FileHandle(forUpdating: url)
        defer { try? file.close() }
        let offset = try file.seekToEnd()
        do {
            try file.write(contentsOf: frame)
            try file.synchronize()
        } catch {
            // The in-memory checkpoint remains unchanged; reopening repairs a torn tail.
            cache.removeValue(forKey: batch.identity.key)
            try? file.truncate(atOffset: offset); try? file.synchronize()
            throw error
        }
        return offset
    }

    private nonisolated static func readFrame(_ file: FileHandle) throws -> Batch? {
        let prefix = try file.read(upToCount: 4) ?? Data()
        guard prefix.count == 4 else { return nil }
        let count = Int(Bytes(prefix).u32(0))
        guard count > 0, count <= 8192 else { throw WearableError.storage("Invalid history frame size.") }
        let payload = try file.read(upToCount: count) ?? Data()
        let checksum = try file.read(upToCount: 4) ?? Data()
        guard payload.count == count, checksum.count == 4 else { return nil }
        guard HistoryCRC.crc32(payload) == Bytes(checksum).u32(0) else { throw WearableError.storage("Saved history checksum failed. No data acknowledged.") }
        return try JSONDecoder().decode(Batch.self, from: payload)
    }

    private func apply(_ batch: Batch, records: [HistoryRecord]?, to session: inout HistorySession) {
        session.nextSequence = batch.end; session.updatedAt = batch.importedAt
        if let records {
            session.recordCount += UInt64(records.count); session.lastTimeUs = records.last?.timeUs
        } else { session.missingCount += UInt64(batch.end - batch.first) }
    }

    private func entries(state: State, afterSequence: Int64, limit: Int) throws -> [HistoryEntry] {
        let input = try FileHandle(forReadingFrom: fileURL(state.session.identity))
        defer { try? input.close() }
        var result: [HistoryEntry] = []
        for index in state.index where Int64(index.end) > afterSequence {
            try input.seek(toOffset: index.offset)
            guard let batch = try Self.readFrame(input) else { throw storage("Saved history is incomplete.") }
            for entry in try Self.makeEntries(batch) where Int64(entry.sequence) > afterSequence {
                result.append(entry)
                if result.count == limit { return result }
            }
        }
        return result
    }

    private nonisolated static func makeEntries(_ batch: Batch) throws -> [HistoryEntry] {
        if let bytes = batch.bytes {
            return try HistoryRecord.decodeBlock(bytes, first: batch.first).map {
                HistoryEntry(sequence: $0.sequence, end: $0.sequence + 1, importedAt: batch.importedAt, record: $0)
            }
        }
        return [HistoryEntry(sequence: batch.first, end: batch.end, importedAt: batch.importedAt, record: nil)]
    }

    private func storage(_ message: String) -> WearableError { .storage(message) }
}
