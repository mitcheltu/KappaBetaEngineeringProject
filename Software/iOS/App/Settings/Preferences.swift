import Foundation
import SwiftUI
import WearableCore

struct Preferences: Codable, Equatable {
    var heartRate = 70.0
    var maxDepth = 2.0
    var weightKg = 70.0
    var weightUnit = WeightUnit.kg
    var heightCm = 170.0

    enum WeightUnit: String, Codable, CaseIterable { case kg, lbs }

    var deviceSettings: WearableSettings {
        WearableSettings(heartRate: heartRate, maxDepthM: maxDepth, weightKg: weightKg, heightCm: heightCm)
    }

    var isValid: Bool {
        heartRate.isFinite && (20...140).contains(heartRate) && heartRate.rounded() == heartRate
            && maxDepth.isFinite && (0.1...10).contains(maxDepth)
            && weightKg.isFinite && (20...150).contains(weightKg)
            && heightCm.isFinite && (80...250).contains(heightCm) && heightCm.rounded() == heightCm
    }
    var displayedWeight: Double {
        get { weightUnit == .kg ? weightKg : weightKg / 0.45359237 }
        set { weightKg = min(150, max(20, weightUnit == .kg ? newValue : newValue * 0.45359237)) }
    }
    var weightRange: ClosedRange<Double> {
        weightUnit == .kg ? 20...150 : (20 / 0.45359237)...(150 / 0.45359237)
    }
}

@MainActor
final class PreferencesStore: ObservableObject {
    @Published var draft: Preferences
    @Published private(set) var saved: Preferences?
    @Published private(set) var message = ""
    private let defaults: UserDefaults
    private let key = "kappa-beta.ios.preferences.v1"
    var isDirty: Bool { saved != draft }

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        if let data = defaults.data(forKey: key),
           let value = try? JSONDecoder().decode(Preferences.self, from: data), value.isValid {
            saved = value
            draft = value
        } else { draft = Preferences() }
    }

    func save() {
        guard draft.isValid, let data = try? JSONEncoder().encode(draft) else {
            message = "Check that all settings are within the displayed ranges."
            return
        }
        defaults.set(data, forKey: key)
        saved = draft
        message = "Saved on this iPhone. These values will be sent after history finishes on the next connection."
    }

    func reset() {
        draft = saved ?? Preferences()
        message = "Edits reset."
    }
}
