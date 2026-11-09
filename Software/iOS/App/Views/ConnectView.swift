import SwiftUI

@MainActor
struct ConnectView: View {
    @ObservedObject var connection: WearableConnection

    var body: some View {
        List {
            Section {
                VStack(alignment: .leading, spacing: 12) {
                    Image(systemName: "water.waves").font(.largeTitle).foregroundStyle(.teal)
                    Text("Your wearable.\nWithin reach.").font(.largeTitle).bold()
                    Text("Connect over Bluetooth. Setup, live readings and saved recordings work without Wi-Fi or mobile data.")
                        .foregroundStyle(.secondary)
                }.padding(.vertical, 12)
            }
            Section("Bluetooth connection") {
                if connection.canConnect {
                    Button("Find wearable", systemImage: "magnifyingglass", action: connection.startScanning)
                } else {
                    Button(connection.state == .connected ? "Disconnect wearable" : "Cancel connection",
                           role: .cancel, action: connection.disconnect)
                }
                if connection.state == .scanning {
                    HStack { ProgressView(); Text("Looking for nearby wearables…") }
                }
                if connection.state == .scanning || connection.canConnect {
                    ForEach(connection.devices) { device in
                        Button { connection.connect(to: device.id) } label: {
                            HStack {
                                VStack(alignment: .leading) {
                                    Text(device.name).font(.headline)
                                    Text(device.id.uuidString.suffix(8)).font(.caption.monospaced())
                                }
                                Spacer()
                                Text("\(device.rssi) dBm").font(.caption)
                            }
                        }
                    }
                }
                if !connection.message.isEmpty { Text(connection.message).font(.callout) }
                Text("Disconnect the WebApp before connecting here. This firmware serves one app at a time.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Surface baseline") {
                LabeledContent("Reference", value: connection.depth?.depthM == nil ? "Not available" : "Available")
                Text("Hold the pressure sensor at the water surface or in air before zeroing.")
                Button(connection.zeroPending ? "Requesting…" : "Zero depth at surface", action: connection.zeroDepth)
                    .disabled(connection.state != .connected || connection.zeroPending)
                Text("A successful write only sends the request. Check subsequent valid depth readings to verify the surface reference.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            if let history = connection.history {
                Section("Recording transfer") {
                    Text(history.phase.label)
                    LabeledContent("Saved records", value: "\(history.recordCount)")
                    LabeledContent("Missing records", value: "\(history.missingCount)")
                    if let target = history.target {
                        LabeledContent("Next record / snapshot end", value: "\(history.cursor) / \(target)")
                    }
                }
            }
            Section("Using your device") {
                Text("Keep this app open while transferring or viewing live data. Moving it to the background disconnects Bluetooth; reconnect to resume from the saved checkpoint.")
                Text("The app cannot start or stop the pump. Use the wearable’s physical controls. Radio communication underwater is not guaranteed.")
            }.font(.callout)
        }
        .navigationTitle("Connect")
    }
}
