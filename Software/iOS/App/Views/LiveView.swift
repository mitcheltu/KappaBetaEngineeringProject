import SwiftUI
import WearableCore

private enum SensorPage: String, CaseIterable { case depth = "Depth", vitals = "Vitals", motion = "Motion", device = "Device", history = "History" }

// A deliberate display snapshot. The connection and durable transfer continue independently.
@MainActor
private struct DisplayReadings {
    var depth: DepthReading?
    var imu: IMUReading?
    var ppg: PPGReading?
    var system: SystemReading?
    init(_ connection: WearableConnection) {
        depth = connection.depth; imu = connection.imu; ppg = connection.ppg; system = connection.system
    }
}

@MainActor
struct LiveView: View {
    @ObservedObject var connection: WearableConnection
    @State private var page = SensorPage.depth
    @State private var frozen: DisplayReadings?
    private var readings: DisplayReadings { frozen ?? DisplayReadings(connection) }

    var body: some View {
        VStack(spacing: 0) {
            Picker("Sensor view", selection: $page) {
                ForEach(SensorPage.allCases, id: \.self) { Text($0.rawValue).tag($0) }
            }.pickerStyle(.menu).padding(.horizontal)
            if page == .history {
                HistoryView(connection: connection)
            } else {
                List {
                    Section {
                        Text(frozen != nil ? "Display paused · Bluetooth transfer continues" : connection.hasStaleReadings ? "Some readings are stale" : connection.state == .connected ? "Receiving sensor data" : "No live data")
                            .font(.callout).foregroundStyle(.secondary)
                        Button(frozen == nil ? "Pause updates" : "Resume updates") {
                            frozen = frozen == nil ? DisplayReadings(connection) : nil
                        }.disabled(connection.state != .connected)
                    }
                    switch page {
                    case .depth: depthSection
                    case .vitals: vitalsSection
                    case .motion: motionSection
                    case .device: deviceSection
                    case .history: EmptyView()
                    }
                    Section {
                        Text("A dash means there is no current valid reading. Readings expire after three seconds without a packet. Paused values are a snapshot.")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
        }
        .navigationTitle(page == .history ? "Your recordings" : "Live readings")
        .onChange(of: connection.state) { _, state in if state != .connected { frozen = nil } }
    }

    private var depthSection: some View {
        Section("Depth from surface") {
            MetricRow(label: "Depth", value: number(readings.depth?.depthM, 3), unit: "m")
            MetricRow(label: "Pressure", value: number(readings.depth?.pressurePa, 0), unit: "Pa")
            MetricRow(label: "Water temperature", value: number(readings.depth?.temperatureC, 2), unit: "°C")
            Text("Depth uses the surface baseline set in Connect.").font(.caption)
        }
    }
    private var vitalsSection: some View {
        Section("Optical sensor") {
            MetricRow(label: "Heart rate", value: number(readings.ppg?.heartRateBpm, 0), unit: "bpm")
            MetricRow(label: "Oxygen estimate", value: number(readings.ppg?.spo2Percent, 1), unit: "%")
            LabeledContent("Raw red", value: readings.ppg?.red.map { String($0) } ?? "—")
            LabeledContent("Raw infrared", value: readings.ppg?.infrared.map { String($0) } ?? "—")
            Text("Experimental sensor estimates requiring a valid optical signal.").font(.caption)
        }
    }
    private var motionSection: some View {
        Group {
            Section("Acceleration · includes gravity") {
                axes(readings.imu?.accelerationG, digits: 3, unit: "g")
            }
            Section("Rotation") { axes(readings.imu?.angularVelocityDps, digits: 1, unit: "°/s") }
        }
    }
    private var deviceSection: some View {
        Section("Device status") {
            MetricRow(label: "Battery", value: number(readings.system?.batteryV, 3), unit: "V")
            LabeledContent("Pump command", value: pumpStatus)
            LabeledContent(readings.system?.vibrationMotor == true ? "Vibration motor" : "Blue LED",
                           value: readings.system.map { $0.vibrationOn ? "On" : "Off" } ?? "—")
            LabeledContent("Countdown", value: readings.system.map { $0.countdownActive ? "\(Int(ceil(Double($0.remainingMs) / 1000))) s" : "Inactive" } ?? "—")
            LabeledContent("SOS", value: readings.system.map { $0.sosPressed ? "Pressed" : "Released" } ?? "—")
            LabeledContent("Cancel", value: readings.system.map { $0.cancelPressed ? "Pressed" : "Released" } ?? "—")
            LabeledContent("Sensor errors", value: readings.system.map { String($0.sensorErrors) } ?? "—")
            LabeledContent("Optical overflows", value: readings.system.map { String($0.ppgOverflowCount) } ?? "—")
            Text("Outputs show GPIO commands, not water flow. Use physical Cancel to stop the pump.").font(.caption)
        }
    }
    private var pumpStatus: String {
        guard let system = readings.system else { return "—" }
        if system.pumpOn { return system.remainingMs > 0 ? "Running · \(Int(ceil(Double(system.remainingMs) / 1000))) s left" : "Running until Cancel" }
        return system.pumpEnabled ? "Stopped" : "Disabled"
    }
    private func axes(_ values: [Double]?, digits: Int, unit: String) -> some View {
        ForEach(0..<3) { index in
            MetricRow(label: ["X", "Y", "Z"][index], value: number(values.flatMap { $0.indices.contains(index) ? $0[index] : nil }, digits), unit: unit)
        }
    }
}

private func number(_ value: Double?, _ digits: Int) -> String {
    guard let value else { return "—" }
    return value.formatted(.number.precision(.fractionLength(digits)))
}

private struct MetricRow: View {
    let label: String
    let value: String
    let unit: String
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(label).font(.subheadline).foregroundStyle(.secondary)
            Text("\(value) \(unit)").font(.title.monospacedDigit()).bold()
        }.padding(.vertical, 6).accessibilityElement(children: .combine)
    }
}
