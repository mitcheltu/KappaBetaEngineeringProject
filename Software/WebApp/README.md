# Kappa Beta Wearable Demo

A responsive wearable instrument UI built with React Native, React Native Web, TypeScript, and Expo. Adjust resting heart rate and estimated depth, tap the ocean depth scale, and explore a simulated Bluetooth connection to an ESP32 wearable.

The controls and connection dock at the top remain a simulation. The separate **ESP32 / Live sensor readings** panel connects to the Rev F firmware through Web Bluetooth and displays real depth, pressure, temperature, IMU, PPG, and device status. It does not run drowning detection or activate the pump.

## Connect the Rev F wearable

1. Build and flash the complete `DrowningWearable` sketch from `Software/`, including `History.ino` and `HistoryBuffer.h`, using the pin assignments in [FirmwareConfig.h](../FirmwareConfig.h) and the [Arduino setup instructions](../../Electrical/README.md#arduino-setup-official-bluerobotics-library), including official **BlueRobotics MS5837 Library 1.1.1**. Power the ESP32 and sensors.
2. Open the webapp on its HTTPS GitHub Pages address, or `http://localhost:8081`, in Chrome on Windows, macOS, or Android. Web Bluetooth requires a secure context and a direct user gesture; a plain HTTP LAN address is not sufficient. Safari/iPhone and browsers without Web Bluetooth are not supported by this connection path. See [Chrome's Web Bluetooth documentation](https://developer.chrome.com/docs/capabilities/bluetooth).
3. In the live panel, select **Connect wearable**, then choose **KappaBeta-RevF** in the browser's device picker. The app saves the initial history snapshot before enabling live readings. The upper **Connect** button is still the explicitly labelled demo connection.
4. With the pressure sensor at the water surface or in air, select **Zero depth at surface**. Depth remains unknown until firmware captures a valid session baseline. Do not zero while the sensor is submerged below the intended surface reference.
5. Read the live sensor cards. Unknown, invalid, uncalibrated, or stale values show `—`; a stream becomes stale after three seconds without a new packet. Disconnecting clears all readings. Heart rate and SpO₂ require a valid optical signal and remain experimental sensor estimates.

Pressure values that pass the firmware's acknowledgement and sanity checks still require reference validation: official MS5837 library 1.1.1 does not report every internal I2C failure and has a compensation-arithmetic limitation. See [pressure reading validation](../../Electrical/README.md#pressure-reading-validation); a displayed reading is not proof of measurement accuracy.

The page receives measurements directly from the device; no server or Wi-Fi credentials are needed. This path does not establish reliable underwater radio communication. BLE writes control history transfer and surface pressure calibration; they cannot activate the pump. Firmware controls pump availability and reports its state.

For the manual bench sequence, the physical **SOS** button starts a 10-second vibration countdown before starting the diaphragm pump on GPIO16. Vibration on GPIO7 stops when the pump starts, and the pump stays commanded running until **Cancel**. Physical Cancel aborts the countdown, stops vibration, and stops the pump. Low or invalid battery voltage also aborts the outputs. The live panel displays the countdown, vibration state, and **Pump running until Cancel** or **Idle · pump stopped**. These are **GPIO command states, not water-flow feedback**. The webapp cannot start, cancel, or operate this sequence; use the physical controls or the firmware's Serial commands. There is no drowning-detection logic.

The service UUID is `e81b6200-7e57-4d8e-b6f4-889c1d4e8270`. Version 1 telemetry uses four 20-byte little-endian characteristics ending in `6210` (depth), `6211` (IMU), `6212` (PPG), and `6213` (system), with the same UUID suffix. Each carries a version byte, validity flags, sequence, and device uptime; payload decoding is in `src/services/telemetryProtocol.ts`. Surface calibration writes byte `1` to `e81b6220-7e57-4d8e-b6f4-889c1d4e8270`. Unsupported packet versions and malformed lengths are reported instead of interpreted as readings.

System flags include bit 1 for pump enablement, bit 2 for the pump command, bit 5 for the vibration command, and bit 6 for an active countdown. The 16-bit value at offset 10 reports countdown milliseconds while bit 6 is set; it is zero while idle or while the pump runs until Cancel. Earlier packets with bits 5 and 6 clear still decode normally. The UUIDs, 20-byte packet layout, and protocol version 1 remain unchanged.

## Local sensor history

The powered wearable continuously records 32-byte sensor records in its PSRAM ring. Its 4 MiB ring holds 131,072 records (about 25.7 minutes at the maximum 85-record/s scheduling budget; blocking sensor reads reduce the actual rate). Old records are overwritten as the ring fills. Reset or power loss clears MCU history. Two 1 MiB workspaces are reserved for future ML; no models run yet.

On connection, the app identifies the device and boot session, reads its local checkpoint, and downloads a fixed snapshot. Blocks contain at most 64 records, support the default Bluetooth MTU, and are checked for fragment bounds, CRC32, and consecutive sequence IDs. Records or explicit missing ranges and the next cursor commit in one IndexedDB transaction. An ACK is sent only after transaction completion, with strict durability requested. A failed transaction, full browser storage, malformed block, or stalled link stops the transfer; reconnect resumes the durable checkpoint. Repeated blocks are idempotent. Overwritten ranges remain visible as gap entries rather than being counted as saved samples.

After END/LIVE confirmation, live sensor cards begin and one background transfer at a time polls from the durable cursor, waiting roughly 250 ms between transfers. This includes samples acquired during the initial download and subsequent full-rate history alongside the 5 Hz live display. Sampling continues on the MCU throughout. A slow or interrupted Bluetooth link can still fall behind ring overwrite; this is not a lossless-retention guarantee. Missing history characteristics require a firmware update. If the device reports no history allocation, the panel explicitly shows **Live only** and does not claim to save history.

Sessions stay in this browser's IndexedDB for this site/origin, keyed by device MAC and boot nonce. They survive a normal page reload and remain exportable offline after the app has loaded. No history is uploaded. Other browsers, devices, or site origins have separate storage. Clearing site data, private browsing, quota limits, or browser eviction can remove it. Transaction completion with a strict durability hint is not an absolute guarantee against storage or power failure. Export important sessions to a file.

The **Sensor history / This browser** section lists saved sessions and exports CSV. Supporting browsers stream the export to a chosen file in bounded pages; the fallback downloads explicit parts of up to 5,000 entries and offers **Export next CSV part**. Exports include gaps, raw optical counts, sensor validity, baseline changes, actuator command state, session/sequence IDs, and device acquisition microseconds. Device timestamps are not calendar times. Raw PPG timestamps are estimated from the FIFO and flagged accordingly; record append order can differ from timestamp order across sensor kinds. `imported_at` is the browser's import time, not acquisition time.

The complete wire contract is [HISTORY_PROTOCOL.md](../HISTORY_PROTOCOL.md). History uses characteristics ending `6230`–`6233`; existing live characteristics and packet layouts remain unchanged. Calibration and history requests share a serialized GATT queue.

## Design and layout

Light paper surfaces, navy ink, lime actions, and square corners frame an interactive ocean depth stage. Locally bundled **Barlow Condensed Bold** provides display typography; **Space Mono Regular** identifies readings and technical labels. `src/fonts.web.ts` loads the stylesheet in `public/fonts/` using Expo's deployment base path; its relative font URLs work both locally and on GitHub Pages. Originals and licenses are retained in `assets/fonts/`. See [font provenance](assets/fonts/README.md) and [ocean asset provenance](assets/README.md).

- **Desktop:** The ocean stage, compact connection dock, and scenario segments sit beside a controls panel with stacked sliders. The sensor strip spans the workspace below.
- **Tablet and phone:** Below a 960 px viewport, the main columns stack. Sliders share a row only when the controls panel has at least 580 px of content width; otherwise they stack. Controls and the three sensor readings reflow at narrow widths.
- **Device picker:** A square, centered dialog becomes a bottom sheet on phones. Its contents scroll in short viewports.

The interface keeps explanation brief and makes the scale, presets, sliders, and connection scenarios directly available. Actual verification coverage is recorded in the audit documents below.

## Run locally

From `Software/WebApp`, with Node.js 24 and npm installed:

```powershell
npm ci
npm run web
```

Open the address printed by Expo, normally [http://localhost:8081](http://localhost:8081). See the official [Expo web development guide](https://docs.expo.dev/workflow/web/) for the web workflow.

To reach the development server from a phone on the same Wi-Fi network:

```powershell
npm run web -- --host lan
```

Open `http://<computer-LAN-IP>:8081` in the phone's browser, using the port printed by Expo if it differs. The computer must remain running and the network must allow access to its development server.

## Explore the demo

- **Resting heart rate:** A user-entered baseline from 20–140 bpm, in steps of 1 bpm, starting at 70. Drag, use the step buttons or keyboard, or tap a 60 / 70 / 90 bpm preset.
- **Estimated depth:** A simulated reading from 0.0–10.0 m, in steps of 0.1 m, starting at 0.0. Use its slider, step buttons, keyboard, or 0 / 2 / 5 m presets.
- **Ocean scale:** Tap 0 / 2 / 4 / 6 / 8 / 10 m. The stage reading, cursor, and depth slider share the same value. These controls work while disconnected.
- **Connection dock:** Select **Connect**, choose “Demo Wearable ESP32,” and wait for the simulated connection. **Disconnect** ends it. The connected sensor strip mirrors estimated depth; IMU and PPG remain placeholders. Use the separate live panel for hardware.
- **Connection test:** The always-visible **NORMAL / EMPTY / FAIL** segments select the outcome of subsequent attempts. Close the picker to change the scenario. Retrying a failure repeats it until the scenario changes.
- **Connection loss:** While connected, select **DROP LINK**. **Reconnect** starts discovery again; **Dismiss** returns to the disconnected state. Current values are preserved.
- **Reset:** Restore 70 bpm and 0.0 m without changing the connection. Connecting, disconnecting, and canceling also preserve values.
- **Reload:** Start a fresh demo with initial values, normal discovery, and no connection. Demo controls are not persisted; real sensor history remains in local browser storage.

The example ranges and presets are UI fixtures, not activation thresholds or physiological guidance. Closing or canceling the picker cancels pending mock work.

## Checks and production export

```powershell
npm test
npm run typecheck
npm run build
```

Tests cover mock and live connection services plus history decoding and transfer: byte order and units, malformed fragments/CRC/record IDs, stale tokens, durable-ACK ordering, lost-ACK retry races, gaps, disconnect/resume, storage failures, timeout/LIVE recovery, single-flight background polls, and no-memory mode. `npm run build` exports to `dist/` for hosting at `/` by default. `dist/` is generated and ignored by Git. These checks do not replace an on-device sensor and Bluetooth test.

`scripts/check-history-browser.mjs` uses an existing Playwright/Chromium installation to exercise real IndexedDB transaction completion, rollback after a record write succeeds, concurrent duplicate commits, persistence across reload, bounded streaming and multipart CSV export, and the built desktop/mobile UI with a fake BLE device. Run after building:

```powershell
node scripts/check-history-browser.mjs <playwright/index.mjs> <chromium-executable>
```

An optional third argument selects the export directory instead of `dist/`. Screenshots go to ignored `.expo/history-check/`.

## GitHub Pages

The repository includes [`.github/workflows/webapp-pages.yml`](../../.github/workflows/webapp-pages.yml). It installs dependencies with Node.js 24, runs the typecheck and tests, reads the repository's Pages configuration, exports the app, and deploys `dist/` through GitHub's Pages artifact actions.

1. In the repository's **Settings → Pages → Build and deployment**, select **GitHub Actions** as the source. See [GitHub's publishing-source instructions](https://docs.github.com/en/pages/getting-started-with-github-pages/configuring-a-publishing-source-for-your-github-pages-site#publishing-with-a-custom-github-actions-workflow).
2. Commit and push `Software/WebApp/` and the workflow to `main`. Changes to either location trigger deployment automatically.
3. For a manual deployment, open **Actions → Deploy WebApp to GitHub Pages → Run workflow** and select `main`. Other branches can build manually but cannot deploy.

The expected project URL is [flwrsfralgernn.github.io/KappaBetaEngineeringProject/](https://flwrsfralgernn.github.io/KappaBetaEngineeringProject/). A successful deploy job reports the actual URL. The workflow obtains the path from `actions/configure-pages` and passes it through `EXPO_BASE_PATH` to `app.config.js`, so a custom domain configured in Pages settings also uses the correct asset paths. See [GitHub's custom-workflow guide](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages).

To build the same project-path export locally:

```powershell
npm run build:pages
```

This defaults to `/KappaBetaEngineeringProject`. To target another path, set `EXPO_BASE_PATH` before building:

```powershell
$env:EXPO_BASE_PATH = '/another-repository'
npm run build:pages
Remove-Item Env:EXPO_BASE_PATH
```

For a root-hosted export, leave `EXPO_BASE_PATH` unset and use `npm run build`. Preview a Pages export at its configured path (for example, `/KappaBetaEngineeringProject/`); serving it at `/` will not resolve its prefixed assets. No separate `gh-pages` branch or committed build output is required.

Local export and browser verification are recorded in [audit/PAGES.md](audit/PAGES.md).

### If `configure-pages` reports `HttpError: Not Found`

This lookup happens before the web export. Verify the one-time setup in [this repository's Pages settings](https://github.com/flwrsfralgernn/KappaBetaEngineeringProject/settings/pages): under **Build and deployment → Source**, select **GitHub Actions**. You need repository admin or maintainer access to change the publishing source. Then return to the failed Actions run and select **Re-run all jobs**. The original run can succeed after the setting is corrected; no app changes are required for this setup error.

The workflow already grants `pages: read` for the configuration lookup. It runs the typecheck and tests before that lookup so setup problems do not prevent those checks. If the setting is already correct, verify the repository's Pages availability and the workflow's access. See [GitHub's publishing-source instructions](https://docs.github.com/en/pages/getting-started-with-github-pages/configuring-a-publishing-source-for-your-github-pages-site).

Adding `enablement: true` alone does not fix setup with the default `GITHUB_TOKEN`: automatic enablement requires a different credential according to the [action's input documentation](https://github.com/actions/configure-pages/blob/v5/action.yml). Use the repository settings for this one-time setup.

## Visual audit

[audit/INSTRUMENT.md](audit/INSTRUMENT.md) is the current square instrument UI audit. The earlier dark ocean redesign is preserved in [audit/REDESIGN.md](audit/REDESIGN.md), and the original prototype in [audit/REPORT.md](audit/REPORT.md). Consult the current report for checks actually performed; older screenshots show earlier designs. Physical phones and native builds remain untested.

Development-only screenshot fixtures:

- [Long device name](http://localhost:8081/?auditName=long) — exercise wrapping in the picker and dock.
- [200% CSS zoom](http://localhost:8081/?auditZoom=2) — inspect layout under enlarged rendering.
- [Both fixtures](http://localhost:8081/?auditName=long&auditZoom=2) — combine the stress cases.

Production exports ignore these query parameters. CSS zoom does not certify browser zoom, operating-system text sizing, or assistive technology behavior. Reload without the parameters to restore ordinary rendering.

## Implementation map

| Location | Purpose |
| --- | --- |
| `src/App.tsx` | Ocean depth stage, shared values, connection dock, scenarios, and sensor strip |
| `src/components/DemoValueSlider.tsx` | Sliders, precise step buttons, and one-tap presets |
| `src/components/DevicePickerSheet.tsx` | Discovery, selection, connection, and recovery UI |
| `src/components/SheetFrame.web.tsx` | Browser dialog, focus handling, and background scroll lock |
| `src/components/RangeInput.web.tsx` | Accessible browser range input |
| `src/services/mockConnection.ts` | Local connection state and cancellable mock delays |
| `src/components/LiveTelemetryPanel.tsx` | Independent hardware connection, measurement cards, and surface zero control |
| `src/services/liveConnection.ts`, `src/services/telemetryProtocol.ts` | Web Bluetooth lifecycle, freshness, and versioned firmware packet decoding |
| `src/services/historyProtocol.ts`, `historyTransfer.ts`, `historyStore.ts`, `historyExport.ts` | Versioned history transfer, durable local checkpoints, bounded persistence/export |
| `src/components/SavedHistoryPanel.tsx` | Download progress, saved sessions, and CSV export |
| `src/theme.ts`, `src/web.css` | Paper/navy palette, square controls, and responsive layout |
| `src/fonts.web.ts`, `public/fonts/`, `assets/fonts/` | Base-path-aware font loading, declarations, served copies, originals, and licenses |
| `app.config.js`, `scripts/build-pages.mjs` | Deployment base path and local GitHub Pages export command |
| `src/auditFixture.web.ts` | Development-only screenshot stress fixtures |
| `assets/ocean-background.png`, `assets/README.md` | Generated ocean image and provenance |

The `.web.tsx` components provide browser behavior; matching base files provide React Native fallbacks. The [specification](SPECIFICATION.md) and [user flow](USER_FLOW.md) describe the current experience.
