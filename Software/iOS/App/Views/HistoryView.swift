import SwiftUI
import WearableCore

@MainActor
struct HistoryView: View {
    @ObservedObject var connection: WearableConnection
    @State private var sessions: [HistorySession] = []
    @State private var selectedKey: String?
    @State private var exportURL: URL?
    @State private var exporting = false
    @State private var errorMessage: String?
    private var selected: HistorySession? { sessions.first { $0.key == selectedKey } ?? sessions.first }

    var body: some View {
        List {
            if let progress = connection.history {
                Section("Transfer") {
                    Text(progress.phase.label)
                    LabeledContent("Saved records", value: String(progress.recordCount))
                    LabeledContent("Missing records", value: String(progress.missingCount))
                }
            }
            if sessions.isEmpty {
                ContentUnavailableView("No saved recordings", systemImage: "waveform.path",
                                       description: Text("Connect to a wearable with history enabled to save its recordings. Demo firmware streams live data only."))
            } else {
                Section("Saved sessions") {
                    Picker("Session", selection: Binding(get: { selected?.key ?? "" }, set: {
                        selectedKey = $0
                        exportURL = nil
                    })) {
                        ForEach(sessions, id: \.key) { session in
                            Text("\(session.createdAt.formatted(date: .abbreviated, time: .shortened)) · \(session.deviceId.suffix(6))")
                                .tag(session.key)
                        }
                    }.disabled(exporting)
                    if let session = selected {
                        LabeledContent("Saved records", value: String(session.recordCount))
                        LabeledContent("Missing records", value: String(session.missingCount))
                        LabeledContent("Next record", value: String(session.nextSequence))
                        LabeledContent("Device", value: session.deviceId)
                        LabeledContent("Boot session", value: session.bootId)
                        Button(exporting ? "Preparing CSV…" : "Export CSV") {
                            Task { await export(session) }
                        }.disabled(exporting)
                        if let exportURL {
                            ShareLink(item: exportURL) { Label("Share CSV / Save to Files", systemImage: "square.and.arrow.up") }
                        }
                    }
                }
            }
            if let errorMessage { Section { Text(errorMessage).foregroundStyle(.red) } }
            Section("About recordings") {
                Text("Recordings stay in this app on this iPhone and can be exported offline. The WebApp keeps a separate copy. Deleting the app removes its saved history. Export important sessions to Files.")
                Text("Missing records indicate data overwritten before download. The wearable’s temporary history is lost when it loses power. Acquisition times are device uptime, not calendar times; session dates show the first import on this phone.")
                Text("Exports include validity flags, raw optical data, calibration events and explicit gaps. Export captures a fixed snapshot of the saved session.")
            }.font(.caption).foregroundStyle(.secondary)
        }
        .task { await refresh() }
        // Throttle refreshes: a transfer may commit several blocks each second.
        .task {
            while !Task.isCancelled {
                do { try await Task.sleep(for: .seconds(2)) } catch { return }
                await refresh()
            }
        }
    }

    @MainActor private func refresh() async {
        do {
            sessions = try await connection.store.listSessions()
            if selectedKey == nil { selectedKey = sessions.first?.key }
        } catch { errorMessage = "Could not read saved history: \(error.localizedDescription)" }
    }

    @MainActor private func export(_ session: HistorySession) async {
        exporting = true
        exportURL = nil
        errorMessage = nil
        defer { exporting = false }
        do {
            let directory = FileManager.default.temporaryDirectory.appending(path: "Exports", directoryHint: .isDirectory)
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let destination = directory.appending(path: "kappa-beta-\(session.deviceId)-\(session.bootId)-\(UUID().uuidString).csv")
            exportURL = try await connection.store.exportCSV(sessionKey: session.key, to: destination)
        } catch { errorMessage = "CSV export failed: \(error.localizedDescription)" }
    }
}
