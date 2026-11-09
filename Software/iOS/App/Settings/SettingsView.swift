import SwiftUI

@MainActor
struct SettingsView: View {
    @ObservedObject var preferences: PreferencesStore
    @ObservedObject var connection: WearableConnection

    var body: some View {
        Form {
            Section {
                Label(preferences.isDirty ? "Unsaved changes" : "Saved on this iPhone",
                      systemImage: preferences.isDirty ? "pencil.circle" : "checkmark.circle")
                Text("Save your settings here. Compatible firmware receives the saved values after history finishes on your next connection. Unsaved edits are never sent.")
                    .font(.callout).foregroundStyle(.secondary)
                Text(connection.settingsSync.description(saved: preferences.saved?.deviceSettings))
                    .font(.callout)
            }
            Section("Resting heart rate") {
                SettingSlider(value: $preferences.draft.heartRate, range: 20...140,
                              step: 1, unit: "bpm", name: "Resting heart rate")
                Text("Your baseline at rest, separate from live optical readings.").font(.caption)
            }
            Section("Estimated maximum depth") {
                SettingSlider(value: $preferences.draft.maxDepth, range: 0.1...10,
                              step: 0.1, unit: "m", name: "Estimated maximum depth")
                Text("Receiving these values does not enable automatic pump activation. Input ranges are not validated operating limits.")
                    .font(.caption)
            }
            Section("Weight") {
                Picker("Units", selection: $preferences.draft.weightUnit) {
                    ForEach(Preferences.WeightUnit.allCases, id: \.self) { Text($0.rawValue).tag($0) }
                }.pickerStyle(.segmented)
                SettingSlider(value: $preferences.draft.displayedWeight, range: preferences.draft.weightRange,
                              step: 0.1, unit: preferences.draft.weightUnit.rawValue, name: "Weight")
            }
            Section("Height") {
                SettingSlider(value: $preferences.draft.heightCm, range: 80...250,
                              step: 1, unit: "cm", name: "Height")
            }
            Section {
                Button("Save to iPhone", action: preferences.save)
                    .disabled(!preferences.isDirty)
                Button("Reset edits", action: preferences.reset)
                    .disabled(!preferences.isDirty)
                if !preferences.message.isEmpty { Text(preferences.message).font(.callout) }
            }
        }
        .navigationTitle("Your settings")
    }
}

private struct SettingSlider: View {
    @Binding var value: Double
    let range: ClosedRange<Double>
    let step: Double
    let unit: String
    let name: String

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("\(value.formatted(.number.precision(.fractionLength(step < 1 ? 1 : 0)))) \(unit)")
                .font(.title2.monospacedDigit()).bold()
            Slider(value: $value, in: range, step: step) { Text(name) }
                .accessibilityValue("\(value.formatted()) \(unit)")
            Stepper("Adjust \(name.lowercased())", onIncrement: {
                value = min(range.upperBound, ((value + step) * 10).rounded() / 10)
            }, onDecrement: {
                value = max(range.lowerBound, ((value - step) * 10).rounded() / 10)
            })
        }.padding(.vertical, 6)
    }
}
