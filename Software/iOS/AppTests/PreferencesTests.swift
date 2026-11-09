import XCTest
@testable import KappaBeta

@MainActor
final class PreferencesTests: XCTestCase {
    func testUnsavedDefaultsAndPersistedSettings() throws {
        let name = "KappaBetaTests.\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: name))
        defer { defaults.removePersistentDomain(forName: name) }
        let first = PreferencesStore(defaults: defaults)
        XCTAssertNil(first.saved)
        XCTAssertTrue(first.isDirty)
        first.draft.heartRate = 82
        first.draft.weightUnit = .lbs
        first.save()
        let reloaded = PreferencesStore(defaults: defaults)
        XCTAssertEqual(reloaded.draft.heartRate, 82)
        XCTAssertEqual(reloaded.draft.weightUnit, .lbs)
        XCTAssertFalse(reloaded.isDirty)
        reloaded.draft.maxDepth = 4
        reloaded.reset()
        XCTAssertEqual(reloaded.draft.maxDepth, 2)
    }

    func testUnitSwitchPreservesPhysicalWeightAndRejectsInvalidSettings() {
        var preferences = Preferences()
        preferences.weightUnit = .lbs
        XCTAssertEqual(preferences.weightKg, 70)
        XCTAssertEqual(preferences.displayedWeight, 70 / 0.45359237, accuracy: 0.00001)
        preferences.displayedWeight = 200
        XCTAssertEqual(preferences.weightKg, 90.718474, accuracy: 0.00001)
        preferences.maxDepth = .nan
        XCTAssertFalse(preferences.isValid)
        preferences.maxDepth = 2
        preferences.heartRate = 70.5
        XCTAssertFalse(preferences.isValid)
    }

    func testOnlySavedProfileIsUsedAndNewSaveNeedsReconnect() throws {
        let name = "KappaBetaTests.\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: name))
        defer { defaults.removePersistentDomain(forName: name) }
        let preferences = PreferencesStore(defaults: defaults)
        XCTAssertNil(preferences.saved?.deviceSettings)
        preferences.save()
        let sent = try XCTUnwrap(preferences.saved?.deviceSettings)
        preferences.draft.heartRate = 82
        XCTAssertEqual(preferences.saved?.deviceSettings, sent)
        let status = SettingsSyncState.synced(sent)
        XCTAssertTrue(status.description(saved: preferences.saved?.deviceSettings).contains("confirmed"))
        preferences.save()
        XCTAssertNotEqual(preferences.saved?.deviceSettings, sent)
        XCTAssertTrue(status.description(saved: preferences.saved?.deviceSettings).contains("next connection"))
    }
}
