import SwiftUI

@MainActor
struct RootView: View {
    @ObservedObject var connection: WearableConnection
    @ObservedObject var preferences: PreferencesStore

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                Image(systemName: connection.state == .connected ? "antenna.radiowaves.left.and.right" : "antenna.radiowaves.left.and.right.slash")
                VStack(alignment: .leading) {
                    Text(connection.deviceName ?? "Kappa Beta").font(.headline)
                    Text(connection.state.label).font(.caption)
                }
                Spacer()
                if connection.hasStaleReadings { Label("Stale data", systemImage: "clock").font(.caption) }
            }
            .padding(.horizontal).padding(.vertical, 10)
            .background(.thinMaterial)
            .accessibilityElement(children: .combine)
            TabView {
                NavigationStack { ConnectView(connection: connection) }
                    .tabItem { Label("Connect", systemImage: "antenna.radiowaves.left.and.right") }
                NavigationStack { SettingsView(preferences: preferences, connection: connection) }
                    .tabItem { Label("Settings", systemImage: "slider.horizontal.3") }
                NavigationStack { LiveView(connection: connection) }
                    .tabItem { Label("Live", systemImage: "waveform.path.ecg") }
            }
        }
    }
}
