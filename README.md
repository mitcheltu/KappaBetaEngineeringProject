# Aquatic Monitoring Wearable Project

This project implements a chest-worn aquatic monitoring prototype using an ESP32-S3.
It measures pressure/depth, motion, and red/infrared optical signals; estimates
heart rate and SpO₂; streams readings over Bluetooth Low Energy; and provides
manual SOS/Cancel control of an external pump circuit.

This repository contains firmware, browser and iOS clients, shared protocol
tests, experimental sensor-fusion code, research notebooks, and hardware files.
There is no automatic drowning detector or sensor-triggered pump deployment in
the current applications or firmware. Research outputs are not deployable models.
Pump status reports a GPIO command rather than measured flow.

## Contents

- [Repository map](#repository-map)
- [Choose what to run](#choose-what-to-run)
- [Dependency files and installation](#dependency-files-and-installation)
- [Run the web app](#run-the-web-app)
- [Use the connected apps](#use-the-connected-apps)
- [Build and deploy the website](#build-and-deploy-the-website)
- [Run the native iOS app](#run-the-native-ios-app)
- [Hardware and wiring](#hardware-and-wiring)
- [Build and upload firmware](#build-and-upload-firmware)
- [Firmware behavior and diagnostics](#firmware-behavior-and-diagnostics)
- [BLE telemetry and saved profiles](#ble-telemetry-and-saved-profiles)
- [Recording, transfer, and storage](#recording-transfer-and-storage)
- [Run the host tests](#run-the-host-tests)
- [Sensor-fusion library](#sensor-fusion-library)
- [Run the research notebooks](#run-the-research-notebooks)
- [Mechanical, electrical, and archived work](#mechanical-electrical-and-archived-work)
- [Troubleshooting](#troubleshooting)

## Repository map

| Path | Purpose |
| --- | --- |
| [Electrical](Electrical/README.md) | Wiring, power assumptions, libraries, and Arduino setup |
| `Electrical/[KB] Sim - Rev E.zip` | Earlier electrical simulation archive |
| `Mechanical/` | SolidWorks base plate, enclosure, and lid parts |
| [Software](Software/README.md) | Software overview and original application concept |
| [DrowningWearable](Software/DrowningWearable/README.md) | Rev F firmware with raw PPG recording and PSRAM history |
| [microcontrollerFinal](Software/microcontrollerFinal/README.md) | Integration sketch with depth helpers and an IMU window |
| [DrowningWearableDemo](Software/testsAndDemo/DrowningWearableDemo/README.md) | Rev I physical-button demo with live-only telemetry |
| [depthSoftware](Software/depthSoftware/README.md) | Standalone pressure/depth and sink-rate experiment |
| [imuSoftware](Software/imuSoftware/README.md) | Standalone BMI270 acquisition and risk-interface experiment |
| [SensorFusion](Software/SensorFusion/README.md) | Portable C++ signal processing and risk-combination library |
| [WebApp](Software/WebApp/README.md) | Expo/React browser client with Web Bluetooth |
| [iOS](Software/iOS/README.md) | Native SwiftUI/CoreBluetooth iPhone and iPad client |
| [tests](Software/tests/README.md) | C++ host tests and shared history wire fixture |
| [GoogleColab](Software/GoogleColab/README.md) | Active physiological-signal research notebooks |
| [Firmware archive](Software/archive/README.md) | Superseded monolithic Arduino sketch |
| [Notebook archive](Software/GoogleColab/archive/README.md) | Superseded research approaches |
| `.github/workflows/` | Web validation/deployment and macOS iOS validation |

`Research/` and `tmp/` are ignored local work areas, not required clone contents.
The local `Research/HeartRateFusion/` workspace contains dataset downloads,
normalization scripts, benchmarks, provenance audits, and generated reports.
`tmp/` holds build artifacts, downloaded toolchains, and test binaries.
The active notebooks run independently of those local work areas.

## Choose what to run

| Goal | Start here | Required tools |
| --- | --- | --- |
| Inspect the browser interface | Web app development server | Node.js 24 and npm |
| Read a real wearable in a browser | Web app plus compatible firmware | Web Bluetooth browser and Bluetooth adapter |
| Read a wearable on iPhone/iPad | Native iOS app | Mac, Xcode, and a physical iOS device |
| Record raw sensor history | `DrowningWearable` | Arduino toolchain and compatible ESP32-S3 hardware |
| Demonstrate vibration and timed pumping | `DrowningWearableDemo` | Rev I wiring and Arduino toolchain |
| Test portable firmware logic | C++ host tests | C++17 compiler |
| Explore physiological rules | Active Colab notebooks | CPU notebook runtime and dataset access |

Commands run from the repository root unless a `cd`/`Set-Location` says otherwise.
PowerShell examples target Windows; iOS examples use a macOS shell.
Each component builds separately. There is no root package installation or
single command that launches all firmware, applications, and research work.

## Dependency files and installation

Dependencies belong in version-controlled manifests for each component;
lockfiles capture resolved versions where supported. Installed dependencies,
SDKs, downloaded datasets, and build artifacts are not committed in their place.

| Component | Dependency file or source | Installation |
| --- | --- | --- |
| Web app | `Software/WebApp/package.json` and `package-lock.json` | Run `npm ci` in that folder |
| Active Arduino sketches | Each sketch's `libraries.txt` | Install the listed versions through Arduino Library Manager |
| ESP32 board/toolchain | Core version and board settings below | Install through Arduino Boards Manager |
| Native iOS | `Software/iOS/Package.swift`; no external Swift packages | Install Xcode/iOS SDK, then open the project |
| Portable C++ | Standard library; BMI270 adapter test also needs its listed Arduino library | Install a host C++17 compiler |
| Research notebooks | First-cell package checks; no `requirements.txt` yet | Use Colab and install packages reported by preflight |

The default web app and active firmware have dependency inventories. Optional
tools are not fully self-contained: the browser harness expects an external
Playwright/Chromium installation, while `esbuild` and Node type declarations
currently arrive through other npm dependencies instead of direct declarations.
The archived firmware also imports `ESP32Servo` without a pinned archive manifest.
Those gaps must be addressed before claiming every optional/archived path can
be reproduced with one dependency-install command. Toolchains remain separate
prerequisites; there is no universal requirements file for this mixed-language repo.

## Run the web app

Use Node.js 24, matching web CI, and install the lockfile versions:

```powershell
Set-Location Software/WebApp
npm ci
npm run web
```

Open the URL printed by Expo, normally `http://localhost:8081`.
The interface loads without a wearable; real measurements require a device.
Use Chrome on a supported desktop or Android device with Bluetooth enabled.
The live connection requires HTTPS or localhost and a user-initiated picker.
iPhone Safari is not the supported Bluetooth route; use the native iOS app.

For layout access from another computer or phone on the same network:

```powershell
npm run web -- --host lan
```

A plain HTTP LAN address does not satisfy Web Bluetooth's secure-context
requirement. Use HTTPS hosting when testing Bluetooth away from localhost.
The main browser entry is `App.web.tsx`, which loads `src/DeviceApp.tsx`.
`App.tsx` loads an older Expo interface with mock connection behavior;
it is separate from the native SwiftUI Bluetooth application in `Software/iOS/`.

| File | Responsibility |
| --- | --- |
| `src/DeviceApp.tsx` | Connect, Settings, Live, and history interface |
| `src/device-app.css` | Browser layout and appearance |
| `src/services/liveConnection.ts` | Connection lifecycle and serialized GATT operations |
| `src/services/telemetryProtocol.ts` | Live packet decoding and units |
| `src/services/profileProtocol.ts` | Saved-profile encoding and readback verification |
| `src/services/historyTransfer.ts` | History transfer state machine |
| `src/services/historyStore.ts` | IndexedDB records and checkpoints |
| `src/services/historyExport.ts` | CSV generation and bounded export |
| `src/services/preferences.ts` | Local profile validation and persistence |

The web stack uses Expo 58, React 19.3, React Native 0.88.1, React Native Web,
and TypeScript 6. Exact constraints are in `package.json`.
There is no application backend, account login, or API key for BLE telemetry.

## Use the connected apps

1. Power a wearable running one of the three supported BLE sketches.
2. Disconnect any other browser or native app using that wearable.
3. Open **Connect**; choose **Connect wearable** (web) or **Find wearable** (iOS).
4. Select the nearby wearable from the Bluetooth picker.
5. Allow the initial recording download and any saved-profile confirmation.
6. Hold the pressure sensor at the surface and choose **Zero depth at surface**.
7. Check subsequent valid depth readings before interpreting relative depth.
8. Open **Live** to inspect Depth, Vitals, Motion, Device, or History.

Surface zero sends a request; write completion alone does not prove calibration
succeeded. Check sensor diagnostics and subsequent valid packets.
Raw pressure and temperature can be available before relative depth is zeroed.
Changing tabs keeps the connection active. **Pause updates** freezes the visible
snapshot while reception and history transfer continue. **Resume updates** shows
current values. Invalid/stale measurements show a dash; streams become stale
after three seconds without a fresh packet. Disconnect clears readings.

Profiles contain resting heart rate, maximum depth, weight, and height.
Choose **Save to browser** or **Save to iPhone** to persist explicit values.
They transfer after the initial history import on the next connection.
Saving while connected takes effect on reconnect. Unsaved drafts and first-use
defaults are not transmitted. Browser and iOS profiles remain independent.
The apps display actuator commands but cannot start or stop the pump.
The physical demo is labeled **Live only**, because it has no recording ring.

## Build and deploy the website

Run service tests, TypeScript checking, and a root-path static export from
`Software/WebApp`:

```powershell
npm test
npm run typecheck
npm run build
```

`npm test` runs `src/services/*.test.ts` through `tsx --test`.
`npm run typecheck` runs `tsc --noEmit`; `npm run build` writes `dist/`.
For one test file: `npx tsx --test src/services/historyTransfer.test.ts`.
Build and preview the GitHub Pages project-path export:

```powershell
npm run build:pages
node scripts/preview-pages.mjs
```

Open `http://localhost:8085/KappaBetaEngineeringProject/`.
The default Pages base path is `/KappaBetaEngineeringProject`.
For a different path, use the same value for build and preview:

```powershell
$env:EXPO_BASE_PATH = '/another-repository'
npm run build:pages
node scripts/preview-pages.mjs
```

Stop the preview with Ctrl+C. Before a root-path build, clear the override:

```powershell
Remove-Item Env:EXPO_BASE_PATH
```

[The Pages workflow](.github/workflows/webapp-pages.yml) validates web changes
on pushes to `main` and supports manual dispatch. It installs dependencies,
checks types, runs tests, exports with the configured Pages path, and deploys.
Set repository **Settings → Pages → Source** to **GitHub Actions** first.
Generated `dist/` is ignored and uploaded as a deployment artifact.

The optional browser harness checks real IndexedDB and fake-BLE UI behavior.
It requires an existing Playwright installation and Chromium beyond `npm ci`:

```powershell
npm run build
node scripts/check-history-browser.mjs '<playwright/index.mjs>' '<chromium-executable>'
```

Replace both placeholders with local paths. Use a root-path export for this
harness; screenshots are written under `.expo/history-check/`.

## Run the native iOS app

The native client targets iOS 17+ on iPhone/iPad. Use a Mac with Xcode 16+
and the iOS SDK. Windows cannot build or run the native application.
The portable `WearableCore` package declares Swift tools 5.9 and macOS 13.
There are no third-party Swift dependencies or application network services.

1. Open `Software/iOS/KappaBeta.xcodeproj` in Xcode.
2. Select the **KappaBeta** scheme and your connected iPhone/iPad.
3. Choose your development team under **Signing & Capabilities**.
4. Change the bundle identifier if your signing setup requires it.
5. Enable Developer Mode if requested, then build and run.
6. Grant Bluetooth access and follow the connection/surface-zero steps above.

After installation, connections, calibration, profiles, live readings, recording
downloads, and CSV sharing work without Wi-Fi/mobile data. Bluetooth is required.
Signing and initial installation may require network access.
The connection is foreground-only: changing tabs preserves it; backgrounding
disconnects it. Reconnect to resume history from the saved cursor.
The wearable continues recording and physical-control operation independently.
A simulator exercises UI/tests but does not verify a physical BLE connection.

Run portable tests on macOS, then list available simulator destinations:

```sh
cd Software/iOS
swift test
xcrun simctl list devices available
```

Replace the destination placeholder with an installed simulator name:

```sh
xcodebuild test -project KappaBeta.xcodeproj -scheme KappaBeta \
  -destination 'platform=iOS Simulator,name=<installed iPhone simulator>' \
  CODE_SIGNING_ALLOWED=NO
```

[The iOS workflow](.github/workflows/ios.yml) runs package and simulator checks
on macOS. Consult actual run results for the revision being used; the workflow
definition alone does not establish a successful native or hardware build.

| Directory | Responsibility |
| --- | --- |
| `App/Views/` | SwiftUI connection, live telemetry, history, and root views |
| `App/Settings/` | Saved preferences and editable drafts |
| `App/Bluetooth/` | CoreBluetooth adapter and history transfer |
| `Sources/WearableCore/` | Protocol codecs, journal storage, and CSV export |
| `Tests/WearableCoreTests/` | Portable protocol and persistence tests |
| `AppTests/` | App-hosted transfer and preference tests |

After app Swift file additions/removals, run `python3 scripts/generate_project.py`
from `Software/iOS` to regenerate project references. With WebApp dependencies
installed, `node scripts/validate-web-fixture.mjs` checks the shared fixture
against the web decoders; it does not execute Swift.

## Hardware and wiring

The main target is an ESP32-S3-DevKitC-1-N8R8 with 8 MB flash and 8 MB PSRAM.
Sensors share a 3.3 V I²C bus. These are GPIO numbers, not header positions.
Check the selected sketch before wiring outputs.

| Function | Main/integration firmware | Rev I demo |
| --- | --- | --- |
| I²C SDA / SCL | GPIO8 / GPIO9 | GPIO8 / GPIO9 |
| BMI270 / MAX30102 interrupt inputs | GPIO4 / GPIO15 | GPIO4 / GPIO15 |
| Cancel / SOS | GPIO5 / GPIO6 | GPIO5 / GPIO6 |
| Vibration gate | GPIO7, held LOW | GPIO7, pulsed |
| Pump gate | GPIO16 | GPIO13 |
| Battery divider ADC | GPIO1 | GPIO1 |
| Countdown indicator | Blue RGB LED, GPIO48 by default | Physical vibration motor |

SOS/Cancel are normally-open switches to ground with input pull-ups.
Battery measurement assumes a 100 kΩ/100 kΩ divider and a factor of two.
The RGB pin is board-revision dependent; DevKitC-1 v1.1 uses GPIO38.

| Sensor | I²C address | Use |
| --- | --- | --- |
| Bosch BMI270 | `0x68` | Three-axis acceleration and angular velocity |
| MAX30102 | `0x57` | Red/IR PPG and experimental HR/SpO₂ estimates |
| MS5837-30BA | `0x76` | Pressure, temperature, and relative depth |

I²C runs at 400 kHz; acquisition polls despite the wired interrupt inputs.
Main firmware uses water density 997 kg/m³; select the intended medium in code.
Pump power comes from an external supply/driver stage, not GPIO power.
Driver current, boost supply, flyback protection, battery calibration, and
waterproofing require physical verification. Rev I documentation requires
removing JP1 when USB is connected. See [Electrical](Electrical/README.md) and
the selected sketch's `FirmwareConfig.h` for wiring/power assumptions.

## Build and upload firmware

Keep all `.ino` tabs and headers in their sketch folder; Arduino combines them.
Install Arduino IDE or CLI and the Espressif `esp32` board package.
Main/integration documentation uses core 3.3.7; demo documents core 3.3.12.
The build helpers use the installed core without installing/enforcing its version.
Install these versions through Arduino Library Manager:

| Library Manager name | Version |
| --- | --- |
| SparkFun BMI270 Arduino Library | 1.0.3 |
| SparkFun MAX3010x Pulse and Proximity Sensor Library | 1.1.2 |
| BlueRobotics MS5837 Library | 1.1.1 |
| NimBLE-Arduino | 2.5.2 |

`libraries.txt` is dependency metadata, not an automatic Arduino IDE installer.
Use the original BlueRobotics library; other libraries also export `MS5837.h`.
Open `Software/DrowningWearable/DrowningWearable.ino` for the main sketch.

| Arduino setting | Value for the N8R8 target |
| --- | --- |
| Board | ESP32S3 Dev Module |
| Flash / partition | 8 MB / Default 8MB |
| PSRAM | OPI PSRAM |
| USB CDC On Boot | Enabled |
| USB Mode | Hardware CDC and JTAG |
| Serial Monitor | 115200 baud, Newline |

Compile the main firmware from the repository root:

```powershell
./Software/build-firmware.ps1
```

It compiles only: no dependency installation or upload. It finds Arduino CLI
on PATH or in Arduino IDE and reads the configured sketchbook/core location.
Output defaults to `tmp/revf-build/build/`. Overrides are `-ArduinoCli`,
`-LibraryDirectory`, `-BlueRoboticsLibraryDirectory`, `-CoreDataDirectory`,
`-BuildDirectory`, and `-VerboseCompiler`.

For Rev I, open `DrowningWearableDemo.ino` in its folder or compile with:

```powershell
./Software/testsAndDemo/DrowningWearableDemo/build.ps1
# Only for hardware with 16 MB flash:
./Software/testsAndDemo/DrowningWearableDemo/build.ps1 -FlashSize 16M
```

Demo output is `tmp/demo-revi/bin/`; it defaults to two compiler workers.
It accepts `-ArduinoCli`, `-ConfigFile`, `-Jobs`, and `-OutputDirectory` and
does not upload. The root helper always selects the main sketch, not the demo.
The demo expects `libraries/BlueRobotics_MS5837_Library` in the configured sketchbook.
To upload, select the correct sketch/settings/current USB port in Arduino IDE
and click **Upload**. Open Serial Monitor at 115200/Newline and enter `status`.
Close competing serial monitors first. Upload/reset clears volatile history.

For other sketches, open the `.ino` matching the folder name, or compile:

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=opi,CDCOnBoot=cdc Software/microcontrollerFinal
```

Replace the final path with `Software/depthSoftware` or `Software/imuSoftware`
for isolated sensor work. Preserve the repository layout: standalone depth
files reference helpers from sibling `microcontrollerFinal`.

## Firmware behavior and diagnostics

| Sketch | Nominal acquisition/history | Manual output sequence |
| --- | --- | --- |
| `DrowningWearable` | 50 Hz IMU, 100 Hz raw PPG, 5 Hz depth; PSRAM history | Blue LED 10 s, GPIO16 pump until Cancel/off |
| `microcontrollerFinal` | 20 Hz IMU, 25 Hz FIFO PPG, 5 Hz depth; PSRAM history | Same LED/GPIO16 sequence |
| `DrowningWearableDemo` | 20 Hz IMU, 25 Hz FIFO PPG, 5 Hz depth; no history | GPIO7 vibration 10 s, GPIO13 pump 60 s |

These are scheduling targets, not measured guarantees. Blocking depth reads
can reduce acquisition rates. Live packets are nominally sent every 200 ms.
Main raw PPG uses no hardware averaging; `PpgDecimator.h` averages groups of
four into the Maxim algorithm's required 25 Hz input. Estimates use 100 samples
and update once per second after about four seconds of uninterrupted valid data.
Integration/demo instead use hardware averaging to produce 25 Hz FIFO pairs.
Raw red/IR may remain available when derived HR/SpO₂ are invalid.

Main/integration firmware keep vibration off. Demo vibration pulses one second
on, half a second off, within a ten-second window; only physical SOS starts it.
Cancel/off stops either output phase. All three require estimated battery
3400–4350 mV; low, invalid, or unknown battery aborts an active sequence.
Below 2500 mV is invalid-low; 2500–3399 is low; above 4350 is invalid-high.
USB does not replace the battery-divider reading. Cancel takes priority.
Holding SOS at boot cannot trigger; release/repress after cancellation or an
interlock. Active presses cannot extend/queue a sequence. Physical control runs
in its own task independently of blocking sensor reads.

| Newline-terminated Serial command | Purpose |
| --- | --- |
| `help`, `status` | Commands and current diagnostics |
| `z` | Request surface zero in the three BLE sketches |
| `off` | Stop the manual sequence |
| `sos` | Start request in main/integration firmware; unavailable in demo |
| `log error/warn/info/debug/off` | Select one log level, for example `log debug` |
| `calibrate gyro`, `clear calibration` | Integration and standalone IMU calibration |
| `start dive`, `stop dive` | Standalone depth baseline/session control |

Default INFO status prints every two seconds; `status` works with logging off.
Queued Serial output does not block actuator control. The integration sketch
has a 60×6 IMU window, but `runCnn()` returns `0.0f`: no trained CNN is present.
Standalone IMU polls at 100 Hz; integration uses 20 Hz, changing window duration.
Standalone/integration depth processing adds Hampel filtering, EMA smoothing,
and regression depth rate; positive rate means downward motion.

## BLE telemetry and saved profiles

Service UUID: `e81b6200-7e57-4d8e-b6f4-889c1d4e8270`.
Characteristic prefixes below share suffix `-7e57-4d8e-b6f4-889c1d4e8270`:

| Prefix | Role |
| --- | --- |
| `e81b6210` | Depth, pressure, temperature |
| `e81b6211` | Acceleration and angular velocity |
| `e81b6212` | Raw red/IR, HR, SpO₂ |
| `e81b6213` | Battery, buttons, actuator commands, diagnostic counters |
| `e81b6220` | Surface zero; write byte `1` |
| `e81b6221` | Optional profile read/write |
| `e81b6230` / `e81b6231` | History identity / status |
| `e81b6232` / `e81b6233` | History data / commands |

Live packets are 20-byte little-endian version-1 structures with validity,
sequence, and uptime. Decode validity before using payload values. System flag
bit 7 distinguishes the physical vibration indicator from the older blue LED.
Profiles use 12 bytes: version/configured bytes, four uint16 fields, and reserved
zero bytes. [SETTINGS_PROTOCOL.md](Software/SETTINGS_PROTOCOL.md) defines offsets.

| Profile field | Accepted range | Wire unit |
| --- | --- | --- |
| Resting heart rate | 20–140, integer | bpm |
| Maximum depth | 0.1–10 m | millimetres |
| Weight | 20–150 kg | tenths of kg |
| Height | 80–250, integer | centimetres |

Clients validate before rounding, write, then require exact readback confirmation.
Pounds are display-only. Missing profile support allows connection with an
unsupported status; failed/mismatched confirmation is an error. Main/integration
profiles persist in RAM until reboot; demo omits them. The next accepted profile
replaces the last one. These UI/protocol limits are not validated operating limits,
and receiving them does not add depth triggers, detection, or pump behavior.

## Recording, transfer, and storage

Main/integration PSRAM history uses 32-byte records: sequence, kind, validity,
64-bit device-relative microsecond timestamps, and payload. Kinds are depth, IMU, PPG, system,
and surface-zero events. Time is device-relative; backdated FIFO times need not
increase across record kinds. Sequence, not timestamp, is the transfer cursor.
The target ring is 4 MiB/131,072 records, with 2 MiB and 1 MiB fallbacks.
Two 1 MiB future-ML workspaces allocate before enabling history; they hold no model.
Large allocations never fall back to internal SRAM. At the main sketch's upper
budget of about 160 records/s, 4 MiB retains roughly 13.7 minutes. Actual rates
and event counts determine retention; older 25 Hz PPG estimates differ.

Connection order is initial history import, LIVE acknowledgement, saved-profile
synchronization, live subscriptions, then background polling about every 250 ms.
Demo completes the same handshake with zero capacity. Missing required history
characteristics cause a firmware-update error. Only one client owns the link.
Transfers use finite snapshots, blocks of up to 64 records, CRC32, fragmentation,
and exclusive next-record cursors. Clients commit records/checkpoints before ACK.
Identical retries are idempotent; corruption fails. Overwritten ranges become
explicit gaps. ACK does not erase records; disconnect does not stop recording.

| Storage | Lifetime |
| --- | --- |
| ESP32 PSRAM | Lost on reset/power loss; oldest records overwritten when full |
| Browser IndexedDB | Local to site origin/profile; clearing or eviction loses it |
| Browser localStorage | Profile/UI preferences, separate from history |
| iOS journal | Checksummed, flushed before ACK, persists across app restarts |
| iOS UserDefaults | Native saved preferences, independent of browser preferences |

iOS history is excluded from backup and removed with the app. There is no cloud
sync. Browser CSV export streams where supported or offers 5,000-entry parts.
iOS share files are temporary until saved to Files. Exports include validity,
raw signals, commands, surface events, and gaps; client import dates are distinct
from device sample time. See [HISTORY_PROTOCOL.md](Software/HISTORY_PROTOCOL.md).

## Run the host tests

Use a C++17 compiler on PATH. The first line below configures standard MSYS2
UCRT64 on Windows; adjust/omit it for another installation. Keep assertions
enabled: do not use `-DNDEBUG`. Run from the repository root:

```powershell
$env:PATH = 'C:/msys64/ucrt64/bin;' + $env:PATH
New-Item -ItemType Directory -Force -Path tmp/revf-tests | Out-Null
$tests = @('bench_sequence', 'diagnostics', 'history', 'ppg_decimator', 'profile_settings')
foreach ($name in $tests) {
    g++ -std=c++17 -Wall -Wextra -Werror -I Software/DrowningWearable "Software/tests/${name}_test.cpp" -o "tmp/revf-tests/$name.exe"
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $name" }
    & "./tmp/revf-tests/$name.exe"
    if ($LASTEXITCODE -ne 0) { throw "Test failed: $name" }
}
g++ -std=c++17 -Wall -Wextra -Werror -I Software/testsAndDemo/DrowningWearableDemo Software/tests/demo_sequence_test.cpp -o tmp/revf-tests/demo-sequence.exe
if ($LASTEXITCODE -ne 0) { throw 'Demo test compilation failed' }
./tmp/revf-tests/demo-sequence.exe
if ($LASTEXITCODE -ne 0) { throw 'Demo test failed' }
g++ -std=c++17 -Wall -Wextra -Werror -I Software/SensorFusion Software/SensorFusion/sensor_fusion.cpp Software/tests/test_sensor_fusion.cpp -o tmp/revf-tests/sensor-fusion.exe
if ($LASTEXITCODE -ne 0) { throw 'Fusion test compilation failed' }
./tmp/revf-tests/sensor-fusion.exe
if ($LASTEXITCODE -ne 0) { throw 'Fusion test failed' }
```

[Test documentation](Software/tests/README.md) covers BMI270 adapter tests with
the installed SparkFun headers, integration-profile tests, and regeneration of
`history-protocol-fixture.json` using `history_fixture.cpp`. The [stock PPG
harness](Software/tests/demo_stock_ppg/README.md) exercises demo FIFO handling.
These tests cover timing/interlocks, protocol layouts, CRC, overwrite, retries,
gaps, and validation; they do not establish physical sensor/actuator performance.
After wire changes, update firmware/web/Swift codecs and shared fixtures together.

## Sensor-fusion library

`Software/SensorFusion/` is standalone C++17, separate from the Arduino builds.
It processes timestamped depth, IMU, and raw PPG windows: depth Hampel/EMA filters,
depth-slope regression, motion/gravity features, and optical/perfusion statistics.
Depth is positive downward; yaw is unavailable. Pulse/SpO₂ extractors and risk
mappings for motion `M`, ascent `A`, HR `H`, and oxygen `O` are supplied callbacks.
With valid inputs and quality `q`, the combination is:

```text
R = (0.37M + 0.33A + 0.15qH + 0.15qO) / (0.70 + 0.30q)
```

Inputs must lie in `[0, 1]`; known `q = 0` differs from missing quality.
Missing mappings/metrics or invalid windows prevent a final score. The library
supplies no validated quality gate, action threshold, or drowning probability.

## Run the research notebooks

The active `Software/GoogleColab/` notebooks are independent CPU experiments.

| Notebook | Purpose | Data |
| --- | --- | --- |
| `02_hr_reliability.ipynb` | Optical HR agreement/quality gate | WildPPG sternum PPG and ECG |
| `03_oxygen_concern.ipynb` | Fixed oxygen-concern rules, delay sweeps, real-desaturation timing | Dry breath-holds; UW/UCSD controlled hypoxemia (four clinical oximeters); synthetic desaturation; optional OpenOximetry |
| `04_pulse_loss_detector.ipynb` | Pulse-lost / dangerous-rhythm detector from PPG (rule, logistic, boosted trees, 1D CNN) | PhysioNet/CinC 2015 expert-labelled ICU alarms; WildPPG healthy sternum PPG for false alarms |

1. Open Google Colab and upload an active `.ipynb`, or open it from GitHub/Drive.
2. Select the default CPU runtime and inspect the first cell's settings.
3. Choose **Runtime → Run all**; authorize Drive if requested.
4. If preflight reports missing packages, install those with `%pip install ...`
   in a separate cell, restart the runtime, and run all cells again.
5. Inspect output tables, figures, and configuration before interpreting results.

Drive outputs default to `MyDrive/KappaBetaML/outputs/`: `model_b` for 02,
`oxygen_concern` for 03, `hr_syncope` for 04. `SAVE_TO_DRIVE=False` or
`KB_SAVE_TO_DRIVE=0` selects runtime storage, which Colab may discard.
Outside Colab, defaults are local `input/`, `outputs/`, and `cache/` paths.
Dependencies include NumPy, pandas, SciPy, Matplotlib, IPython; model work adds
scikit-learn/joblib, and workbook download work adds openpyxl/requests.

| Environment settings | Purpose |
| --- | --- |
| `WILDPPG_INPUT`, `WILDPPG_OUTPUT`, `WILDPPG_CACHE` | HR-quality storage |
| `WILDPPG_QUICK`, `WILDPPG_ONLINE_IDS`, `WILDPPG_HOP_SECONDS` | HR-quality run size |
| `CONCERN_DATA_ROOT`, `CONCERN_OUTPUT_DIR`, `CONCERN_CACHE` | Oxygen-rule storage |
| `HRSYNC_DATA_ROOT`, `HRSYNC_OUTPUT_DIR`, `HRSYNC_CACHE` | HR-risk storage |
| `WILDPPG_ALLOW_DOWNLOAD`, `CONCERN_DOWNLOAD`, `HRSYNC_DOWNLOAD` | Download permission |
| `HRSYNC_ADJUDICATED_CSV`, `HRSYNC_WORKERS` | External labels and worker count |

Notebook 02 downloads about 5.2 GB; its default Colab download cache stays on
runtime disk even with Drive outputs. Notebook 04 caches processed recordings.
Preserve checksum/ETag verification and original files; investigate changed sources.
04 skips training without sufficient investigator/clinician labels. Its CSV needs
`subject,onset_s,source`; unlisted recordings are excluded, and a labeled blank
onset denotes a negative. Automatic HR-collapse detections are review candidates.
03's synthetic delays are assumptions, not measured chest-sensor performance.
02's documented development run found poor HR agreement and an inadequate gate.
Exports remain `deployable: false`; none is wired into the pump. See [the notebook
README](Software/GoogleColab/README.md) for provenance, results, and limitations.

## Mechanical, electrical, and archived work

Open `Mechanical/tt_base_plate.SLDPRT`, `tt_electronics_enclosure.SLDPRT`, and
`tt_electronics_lid_v1.SLDPRT` with SolidWorks or a compatible CAD viewer.
These part files alone do not supply an assembly or verify waterproofing.
The Rev E simulation ZIP is historical; use the selected sketch's current pins.
Archived firmware/notebooks preserve superseded work and are not default targets.
The original software concept is not a checklist of implemented features.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| `npm ci` fails | Use Node 24 inside `Software/WebApp` and retain the lockfile |
| Bluetooth unavailable | Supported browser, HTTPS/localhost, enabled adapter, picker button |
| Wearable absent | Power/advertising; disconnect the other client |
| Firmware-update message | All four history characteristics must exist |
| Demo has no recordings | Expected live-only behavior |
| Profile pending | Save explicitly and reconnect; demo has no profile support |
| Depth unavailable | Zero at surface; repeat after sensor reinitialization |
| HR/SpO₂ unavailable | Stable optical contact and an uninterrupted valid four-second window |
| SOS rejected | `status`, battery divider, Cancel, release-to-rearm state |
| Wrong output activates | Match GPIO16/GPIO13 wiring to the selected sketch |
| No Serial logs | CDC On Boot, current USB port, 115200 baud/Newline |
| History lost/gapped | Reset clears PSRAM; slow clients can fall behind ring retention |
| Storage stops transfer | Free storage and reconnect; unsaved data is not ACKed |
| iOS backgrounds/disconnects | Expected; reopen and reconnect to resume |
| Pages assets return 404 | Rebuild with the correct `EXPO_BASE_PATH` |
| Notebook verification fails | Restore pinned originals or investigate source changes |

Generated dependencies/builds and local research are ignored. `.gitignore`
allows `README.md` files and the two shared protocol Markdown files;
other Markdown filenames require an explicit exception to be tracked.
