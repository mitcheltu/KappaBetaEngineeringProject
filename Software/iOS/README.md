# Kappa Beta iOS

Native SwiftUI app for iPhone and iPad running iOS 17 or later. It talks directly to the wearable using CoreBluetooth. Once installed, connecting, calibration, settings, live readings, recording downloads and CSV sharing require neither mobile data nor Wi-Fi. Bluetooth must remain enabled. There are no network services, downloaded UI resources or third-party Swift dependencies.

## Open and run

1. On a Mac with Xcode 16 or later and the iOS SDK installed, open `KappaBeta.xcodeproj`. Select the **KappaBeta** scheme.
2. For a physical iPhone, select your development team under **Signing & Capabilities** and change the bundle identifier if necessary. Select your connected iPhone, enable Developer Mode if requested by iOS, and run. Installation/signing and developer-account setup may require internet access; normal use after installation does not.
3. Power the wearable running `Software/DrowningWearable`, `Software/microcontrollerFinal`, or `Software/testsAndDemo/DrowningWearableDemo`. Disconnect the WebApp first.
4. Tap **Connect → Find wearable**, grant Bluetooth access, and select the nearby device. Allow the initial history download to complete. The demo enters **Live only** because it does not record history.
5. Hold the pressure sensor at the water surface or in air and tap **Zero depth at surface**. A successful write confirms only delivery of the request; inspect subsequent valid readings and firmware diagnostics to verify the baseline.
6. View **Settings** or **Live**. Under **Live → History**, export a session, then choose **Share CSV / Save to Files**. Saved sessions remain available while disconnected.

The simulator can run the UI and tests but does not validate a real wearable connection. A Mac, Apple SDK and physical device are required for native build and Bluetooth validation; this initial implementation was authored on Windows and has not yet been compiled in Xcode or exercised against hardware.

## Matching the WebApp

| WebApp function | Native iOS implementation |
| --- | --- |
| Connect, cancel, disconnect, persistent connection status | CoreBluetooth discovery and a single app-owned connection shared by all tabs |
| Surface calibration | Write byte `1` to the existing zero-depth characteristic |
| Heart rate, maximum depth, weight in kg/lbs, height | Validated drafts, save/reset using UserDefaults, and verified transfer of saved values after initial history on the next connection; defaults remain unsaved until explicitly saved |
| Depth, pressure, temperature | Version 1 packets with validity and baseline flags |
| Vitals and raw optical counts | Heart-rate/SpO₂ estimates and raw red/infrared readings |
| Motion | Three-axis acceleration and angular velocity |
| Device | Battery, pump command, countdown, LED/motor, buttons, overflow and sensor-error counters |
| Pause live display | Snapshot of visible readings; Bluetooth/history transfer continues |
| History and export | Durable local checkpoints, gaps, reconnect/resume, session selection, streamed CSV and native sharing |

Unavailable/invalid readings show a dash. A stream becomes stale after three seconds without a new packet, using a monotonic clock. Disconnect clears the displayed measurements. Malformed lengths, unsupported versions, nonfinite floats, bad fragment ranges, conflicting retries and CRC failures are rejected.

Settings have the same bounds as the current WebApp: 20–140 bpm, 0.1–10 m, 20–150 kg (with pounds conversion), and 80–250 cm. These are profile values, not validated device operating limits. **Save to iPhone** persists them locally. After the initial history snapshot finishes on the next connection, the app sends the latest saved profile to compatible firmware and reads it back to verify exact acceptance before marking the connection ready or starting background history polls. Unsaved drafts and first-launch defaults are never sent. Saving while already connected takes effect on the next connection. The Settings page distinguishes confirmed values, pending changes, unsupported firmware, and failures.

Receiving settings does not enable drowning detection or automatic pump activation. The app does not remotely operate the pump. Use the wearable’s physical controls; displayed output states are GPIO commands rather than measured water flow. Browser and iPhone profiles remain separate, so connecting either app applies that app's saved profile.

## Firmware compatibility and switching apps

**One app at a time is supported**, as requested. The iOS app uses the shared GATT contract:

| Role | UUID first group |
| --- | --- |
| Service | `e81b6200` |
| Depth / IMU / PPG / system | `e81b6210`–`e81b6213` |
| Surface zero | `e81b6220` |
| Optional profile settings (read/write) | `e81b6221` |
| History identity / status / data / command | `e81b6230`–`e81b6233` |

All UUIDs end in `-7e57-4d8e-b6f4-889c1d4e8270`. Telemetry is 20-byte, little-endian protocol version 1; history records are 32 bytes. See [the shared wire contract](../HISTORY_PROTOCOL.md).

Settings use a 12-byte little-endian packet: version `1` at byte 0, configured flag `1` at byte 1, resting heart rate (uint16 bpm) at byte 2, maximum depth (uint16 millimetres) at byte 4, weight (uint16 tenths of kg) at byte 6, height (uint16 cm) at byte 8, and reserved zero (uint16) at byte 10. Depth and weight are rounded only after validating the original values. A fresh unconfigured firmware read has version `1` followed by eleven zero bytes. The default saved profile encodes as `[1,1,70,0,208,7,188,2,170,0,0,0]`. A successful GATT write alone does not mark settings as confirmed; readback must match the entire packet. An absent settings characteristic (older/demo firmware) skips transfer and shows an unsupported message while retaining normal history/live behavior. A present characteristic lacking read/write support, a failed operation, or mismatched readback stops connection setup visibly.

`DrowningWearable/History.ino` and `microcontrollerFinal/History.ino` assign history to one connection owner and disconnect additional clients. Demo firmware enforces the same single-client behavior in `Bluetooth.ino`. Disconnect the current app before switching. The device restarts advertising on disconnect. ACKs do not erase MCU records, so each app can download its own copy while those records remain in the ring. iOS data and browser data are independent; there is no automatic synchronization between them.

History-less demo firmware completes the same OPEN/END/LIVE handshake and is explicitly labeled **Live only**. Firmware missing the history characteristics is rejected with an update message rather than silently claiming to save data.

## Offline storage and lifecycle

The `HistoryStore` actor writes checksummed append-journal frames under Application Support. Each frame contains records or an explicit gap and its exclusive next cursor. A commit flushes using `FileHandle.synchronize()` before the transport sends ACK. On reopening, complete frames are CRC/format/continuity checked, incomplete final writes are truncated, and corrupt complete frames stop the transfer. Repeated identical records are idempotent; changed retries are rejected. The store reads/writes bounded frames and exports CSV on a utility task from a fixed snapshot. A per-batch offset index grows with retained history; samples are not loaded wholesale into memory.

The history directory is excluded from device backup and protected on iOS until the first unlock after restart. Sessions survive app restarts but not app deletion. Storage failures stop transfer without acknowledging unsaved data. Filesystem flushing is not an absolute guarantee against hardware or power failure. Export important recordings. Export files are temporary share files; save them to Files for retention. The app has no cloud upload or cloud sync.

The app deliberately uses a foreground connection. Switching tabs keeps it active; moving the app to the background disconnects and cancels pending Bluetooth operations. Reopen and reconnect to resume from the durable checkpoint. The app does not claim continuous monitoring while backgrounded. Firmware recording and physical controls continue independently; power loss clears the device's volatile ring, and ring overwrite can cause visible gaps. Underwater radio reliability and lossless recording are not guaranteed.

History acquisition timestamps are exact device microseconds, not calendar time. Session/import dates are the iPhone’s import times. CSV columns match the WebApp, including validity, raw PPG, surface changes, actuator states, gaps and estimated PPG timestamp flags.

## Validation

On macOS, from this directory:

```sh
swift test
xcodebuild test -project KappaBeta.xcodeproj -scheme KappaBeta \
  -destination 'platform=iOS Simulator,name=<installed iPhone simulator>' \
  CODE_SIGNING_ALLOWED=NO
```

Choose an installed simulator name from `xcrun simctl list devices available`. The [iOS workflow](../../.github/workflows/ios.yml) runs package tests and app-hosted simulator tests on macOS; adding the workflow does not mean those checks have already run.

Checks run on Windows during initial implementation: existing C++ firmware history tests passed; a newly compiled C++ protocol fixture matched the Swift test fixture; `node scripts/validate-web-fixture.mjs` passed against the current WebApp decoders; plist/scheme XML, Xcode source references, deterministic project generation and whitespace checks passed. The Node check needs the WebApp dependencies installed (`npm ci` in `Software/WebApp`). These checks do not compile or execute Swift. The Swift tests and native app still need the macOS checks above, including the newly added settings codec and synchronization tests.

Tests cover telemetry units/invalid packets, fragment/CRC validation, durable commits, torn-tail recovery, corruption, idempotent retries, gaps, CSV data, local settings, and transfer behavior including ACK ordering, lost-ACK retries, stale tokens, no-memory mode and connection cancellation. Settings tests cover the shared wire vector, bounds and rounding, invalid original values, malformed responses, write/read ordering, exact readback, missing saved profiles, older firmware, transport failures, stale connections, and saved-versus-draft behavior. The portable package is separate from Apple frameworks so its protocol/storage logic can be tested directly with SwiftPM.

Before relying on a physical build, perform this bench check with Wi-Fi and cellular data disabled and Bluetooth enabled:

1. Grant/deny Bluetooth permission, turn Bluetooth off/on, scan, cancel, and connect. Confirm timeout/retry messages and no stale values survive disconnect.
2. Test every sensor view, display pause/resume, surface zero, and physical SOS/Cancel output reporting. Confirm no remote pump control is present.
3. Save all four profile settings, terminate/reopen, and confirm local persistence. Connect and verify settings are confirmed only after initial history finishes. Edit without saving and reconnect to confirm the saved values are sent. Save while connected and confirm the new values wait for reconnection. Check that a fresh unsaved profile sends nothing and older/demo firmware reports unsupported settings while still connecting. Verify no automatic pump behavior was introduced.
4. Download history, disconnect during a block, reconnect, and confirm checkpoint resume without duplicated records. Test both history-enabled and demo live-only firmware.
5. Export while offline; inspect CSV gaps, raw fields and timestamps. Test low storage and corrupt journal handling with disposable test data.
6. Disconnect iOS, connect the WebApp and verify telemetry/history, then reverse the handoff. Confirm each client retains its own sessions.
7. Background and relaunch the app; confirm the connection closes and resumed history is honest about overwritten samples.

## Structure

- `App/`: SwiftUI screens, local profile store and CoreBluetooth adapter; UI state is main-actor isolated.
- `App/Bluetooth/HistoryTransfer.swift`: Protocol state machine with serialized writes, timeouts, duplicate handling and commit-before-ACK.
- `Sources/WearableCore/`: Foundation-only telemetry/history/settings codecs, settings write/read verification, durable store and CSV exporter.
- `Tests/WearableCoreTests/`: Portable protocol/storage tests and wire fixtures.
- `AppTests/`: App-hosted transfer and preferences tests.
- `KappaBeta.xcodeproj`: Checked-in project and shared scheme; no project-generation tool is required to open it.
- `scripts/generate_project.py`: Deterministically regenerate project file references after adding/removing app or app-test Swift files. Run `python3 scripts/generate_project.py` and include the resulting project change.

Apple references: [CoreBluetooth central manager](https://developer.apple.com/documentation/corebluetooth/cbcentralmanager), [Bluetooth permission key](https://developer.apple.com/documentation/bundleresources/information-property-list/nsbluetoothalwaysusagedescription), and [SwiftUI scene lifecycle](https://developer.apple.com/documentation/swiftui/scenephase).
