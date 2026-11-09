import SwiftUI
import WearableCore

@main
@MainActor
struct KappaBetaApp: App {
    @StateObject private var model = AppModel()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            Group {
                if let connection = model.connection {
                    RootView(connection: connection, preferences: model.preferences)
                } else {
                    ContentUnavailableView {
                        Label("Local storage unavailable", systemImage: "externaldrive.badge.exclamationmark")
                    } description: {
                        Text(model.errorMessage ?? "Opening local recordings…")
                    } actions: {
                        Button("Try again", action: model.load)
                    }
                }
            }
                .tint(.teal)
                .task { model.load() }
                .onChange(of: scenePhase) { _, phase in
                    if phase == .background { model.connection?.suspend() }
                }
        }
    }
}

@MainActor
private final class AppModel: ObservableObject {
    let preferences = PreferencesStore()
    @Published var connection: WearableConnection?
    @Published var errorMessage: String?

    func load() {
        guard connection == nil else { return }
        do {
            let directory = URL.applicationSupportDirectory.appending(path: "WearableHistory", directoryHint: .isDirectory)
            let preferences = self.preferences
            connection = WearableConnection(store: try HistoryStore(directory: directory),
                savedSettings: { [weak preferences] in preferences?.saved?.deviceSettings })
        } catch { errorMessage = error.localizedDescription }
    }
}
