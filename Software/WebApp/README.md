# Kappa Beta Wearable Web App

A responsive ESP32 wearable interface built with TypeScript, React and Expo. The GitHub Pages web build uses three full-page tabs: **Connect**, **Settings**, and **Live**. A persistent connection indicator remains visible on every tab, and changing tabs keeps the real Web Bluetooth connection and background history transfer active.

The web interface has no simulated device picker, test scenarios or fabricated readings. It connects to Rev F firmware for depth, pressure, temperature, IMU, PPG and device status. Settings currently save only in this browser: the firmware has no command for receiving resting-heart-rate or maximum-depth settings. The app does not run drowning detection or activate the pump.

This redesign targets the web build deployed to GitHub Pages. The React Native fallback retains the previous interface; it has not been redesigned or verified as a native Bluetooth app.

## Connect the Rev F wearable

1. Open [DrowningWearable.ino](../DrowningWearable/DrowningWearable.ino) in Arduino IDE; all firmware tabs and headers are already together in `Software/DrowningWearable/`. Install the dependencies and select the board settings in the [Arduino setup instructions](../../Electrical/README.md#arduino-setup-official-bluerobotics-library), including official **BlueRobotics MS5837 Library 1.1.1**, then select the board's USB port and click **Upload**. Use the pin assignments in [FirmwareConfig.h](../DrowningWearable/FirmwareConfig.h), and power the ESP32 and sensors.
2. Open the webapp on its HTTPS GitHub Pages address, or `http://localhost:8081`, in Chrome on Windows, macOS, or Android. Web Bluetooth requires a secure context and a direct user gesture; a plain HTTP LAN address is not sufficient. Safari/iPhone and browsers without Web Bluetooth are not supported by this connection path. See [Chrome's Web Bluetooth documentation](https://developer.chrome.com/docs/capabilities/bluetooth).
3. In **Connect**, select **Connect device**, then choose **KappaBeta-RevF** in the browser's device picker. The app saves the initial history snapshot before enabling live readings. The persistent indicator distinguishes selection, connection, history syncing, connected, lost-link and error states. **Cancel** stops an in-progress connection; **Disconnect** ends a connected session.
4. With the pressure sensor at the water surface or in air, select **Zero depth at surface**. This sends a calibration request; completion of the Bluetooth write is not confirmation that calibration succeeded. Check subsequent valid depth readings and firmware Serial diagnostics. The UI marks the surface reference **Available** when valid, fresh depth data is present. Depth remains unknown until firmware captures a valid session baseline. Do not zero while the sensor is submerged below the intended surface reference.
5. Open **Live** and choose **Depth**, **Vitals**, **Motion**, **Device**, or **History**. Unknown, invalid, uncalibrated, or stale values show `—`; a stream becomes stale after three seconds without a new packet. Disconnecting clears all readings. Heart rate and SpO₂ require a valid optical signal and remain experimental sensor estimates.

Pressure values that pass the firmware's acknowledgement and sanity checks still require reference validation: official MS5837 library 1.1.1 does not report every internal I2C failure and has a compensation-arithmetic limitation. See [pressure reading validation](../../Electrical/README.md#pressure-reading-validation); a displayed reading is not proof of measurement accuracy.

The page receives measurements directly from the device; no server or Wi-Fi credentials are needed. This path does not establish reliable underwater radio communication. BLE writes control history transfer and surface pressure calibration; they cannot activate the pump. Firmware controls pump availability and reports its state.

For the manual bench sequence, the physical **SOS** button starts a 10-second vibration countdown before starting the diaphragm pump on GPIO16. Vibration on GPIO7 stops when the pump starts, and the pump stays commanded running until **Cancel**. Physical Cancel aborts the countdown, stops vibration, and stops the pump. Low or invalid battery voltage also aborts the outputs. **Live → Device** displays the countdown, vibration state and pump command state. These are **GPIO command states, not water-flow feedback**. The webapp cannot start, cancel, or operate this sequence; use the physical controls or the firmware's Serial commands. There is no drowning-detection logic.

The service UUID is `e81b6200-7e57-4d8e-b6f4-889c1d4e8270`. Version 1 telemetry uses four 20-byte little-endian characteristics ending in `6210` (depth), `6211` (IMU), `6212` (PPG), and `6213` (system), with the same UUID suffix. Each carries a version byte, validity flags, sequence, and device uptime; payload decoding is in `src/services/telemetryProtocol.ts`. Surface calibration writes byte `1` to `e81b6220-7e57-4d8e-b6f4-889c1d4e8270`. Unsupported packet versions and malformed lengths are reported instead of interpreted as readings.

System flags include bit 1 for pump enablement, bit 2 for the pump command, bit 5 for the vibration command, and bit 6 for an active countdown. The 16-bit value at offset 10 reports countdown milliseconds while bit 6 is set; it is zero while idle or while the pump runs until Cancel. Earlier packets with bits 5 and 6 clear still decode normally. The UUIDs, 20-byte packet layout, and protocol version 1 remain unchanged.

## Local sensor history

The powered wearable continuously records 32-byte sensor records in its PSRAM ring. Its 4 MiB ring holds 131,072 records (about 25.7 minutes at the maximum 85-record/s scheduling budget; blocking sensor reads reduce the actual rate). Old records are overwritten as the ring fills. Reset or power loss clears MCU history. Two 1 MiB workspaces are reserved for future ML; no models run yet.

On connection, the app identifies the device and boot session, reads its local checkpoint, and downloads a fixed snapshot. Blocks contain at most 64 records, support the default Bluetooth MTU, and are checked for fragment bounds, CRC32, and consecutive sequence IDs. Records or explicit missing ranges and the next cursor commit in one IndexedDB transaction. An ACK is sent only after transaction completion, with strict durability requested. A failed transaction, full browser storage, malformed block, or stalled link stops the transfer; reconnect resumes the durable checkpoint. Repeated blocks are idempotent. Overwritten ranges remain visible as gap entries rather than being counted as saved samples.

After END/LIVE confirmation, live sensor cards begin and one background transfer at a time polls from the durable cursor, waiting roughly 250 ms between transfers. This includes samples acquired during the initial download and subsequent full-rate history alongside the 5 Hz live display. Sampling continues on the MCU throughout. A slow or interrupted Bluetooth link can still fall behind ring overwrite; this is not a lossless-retention guarantee. Missing history characteristics require a firmware update. If the device reports no history allocation, the panel explicitly shows **Live only** and does not claim to save history.

Sessions stay in this browser's IndexedDB for this site/origin, keyed by device MAC and boot nonce. They survive a normal page reload and remain exportable offline after the app has loaded. No history is uploaded. Other browsers, devices, or site origins have separate storage. Clearing site data, private browsing, quota limits, or browser eviction can remove it. Transaction completion with a strict durability hint is not an absolute guarantee against storage or power failure. Export important sessions to a file.

**Live → History** presents one saved session at a time, with previous/next controls, saved-record count, missing-record count and **Export CSV**. Its **?** control explains storage and shows the selected device and boot-session identifiers. Supporting browsers stream the export to a chosen file in bounded pages; the fallback downloads explicit parts of up to 5,000 entries and offers **Export next CSV part**. Export continuation is retained while switching tabs or sensor views. Exports include gaps, raw optical counts, sensor validity, baseline changes, actuator command state, session/sequence IDs, and device acquisition microseconds. Device timestamps are not calendar times. Raw PPG timestamps are estimated from the FIFO and flagged accordingly; record append order can differ from timestamp order across sensor kinds. `imported_at` is the browser's import time, not acquisition time.

The complete wire contract is [HISTORY_PROTOCOL.md](../HISTORY_PROTOCOL.md). History uses characteristics ending `6230`–`6233`; existing live characteristics and packet layouts remain unchanged. Calibration and history requests share a serialized GATT queue.

## Design and layout

Light paper surfaces, navy ink, lime actions and square controls frame ocean imagery and large readings. Locally bundled **Barlow Condensed Bold** provides display typography; **Space Mono Regular** identifies selected readings and technical labels. `src/fonts.web.ts` loads the stylesheet in `public/fonts/` using Expo's deployment base path; its relative font URLs work both locally and on GitHub Pages. Originals and licenses are retained in `assets/fonts/`. See [font provenance](assets/fonts/README.md) and [ocean asset provenance](assets/README.md).

- **Desktop:** Main tabs sit beneath the persistent header. Connect pairs the ocean stage with the device and baseline controls; Settings shows both input cards side by side; Live shows the selected sensor category.
- **Tablet and phone:** At 760 px and below, the main tabs move to the bottom. Settings presents one input at a time using **Heart rate / Max. depth** controls. Live switches among five sensor views, and History paginates sessions instead of displaying a long list.
- **Readable sizing:** Body and control text starts at 16 px, with larger headings and readings. Standard layouts target a single viewport. Short landscape windows and zoom may scroll the active pane to keep controls accessible; they do not collapse text to fit.
- **Interactions:** Buttons have hover, press and keyboard-focus feedback. Tab content and help dialogs use short transitions, with reduced-motion preferences respected. The main tabs support arrow keys, Home and End. Bluetooth device selection uses the browser's real picker.

Question-mark controls keep detailed variable and connection explanations available without filling the main screens with instructions. Earlier audit screenshots describe previous designs, not this tabbed redesign.

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

## Set initial variables

- **Resting heart rate:** A user-entered baseline from 20–140 bpm in steps of 1 bpm. It is separate from the live optical heart-rate reading.
- **Estimated maximum depth:** A user-entered setting from 0.1–10.0 m in steps of 0.1 m. It is separate from the live measured depth. The intended future behavior is wearable activation above this threshold, but current firmware neither receives nor enforces it.
- **First visit:** 70 bpm and 2.0 m appear as unsaved drafts. These values are not automatic device settings.
- **Save settings:** Saves both values to this browser's localStorage. The page explicitly states that they are not applied to the wearable. Reloading restores saved values; unsaved changes are discarded.
- **Reset edits:** Restores the last saved values, or the initial 70 bpm / 2.0 m drafts if nothing has been saved. It does not change the Bluetooth connection.
- **Input help:** Each **?** button explains the value and the distinction between local settings and sensor readings. Sliders also support step buttons and keyboard adjustment.

These input ranges are unvalidated UI limits, not device operating limits or physiological guidance. Firmware integration and validation are required before the settings can control activation. Settings localStorage and sensor-history IndexedDB are separate stores on the same site origin.

## Checks and production export

```powershell
npm test
npm run typecheck
npm run build
```

Tests cover live connection services and history decoding and transfer: byte order and units, malformed fragments/CRC/record IDs, stale tokens, durable-ACK ordering, lost-ACK retry races, gaps, disconnect/resume, storage failures, timeout/LIVE recovery, single-flight background polls, and no-memory mode. Legacy mock-service tests remain for the native fallback. `npm run build` exports to `dist/` for hosting at `/` by default. `dist/` is generated and ignored by Git. These checks do not replace an on-device sensor and Bluetooth test.

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
node scripts/preview-pages.mjs
```

The preview serves the built app at [http://localhost:8085/KappaBetaEngineeringProject/](http://localhost:8085/KappaBetaEngineeringProject/), matching the default Pages base path. This is a local preview and does not push or deploy anything. Stop it with Ctrl+C.

The build defaults to `/KappaBetaEngineeringProject`. To target another path, set `EXPO_BASE_PATH` before building and previewing:

```powershell
$env:EXPO_BASE_PATH = '/another-repository'
npm run build:pages
node scripts/preview-pages.mjs
Remove-Item Env:EXPO_BASE_PATH
```

For a root-hosted export, leave `EXPO_BASE_PATH` unset and use `npm run build`. Preview a Pages export at its configured path (for example, `/KappaBetaEngineeringProject/`); serving it at `/` will not resolve its prefixed assets. No separate `gh-pages` branch or committed build output is required.

Local export and browser verification are recorded in [audit/PAGES.md](audit/PAGES.md).

### If `configure-pages` reports `HttpError: Not Found`

This lookup happens before the web export. Verify the one-time setup in [this repository's Pages settings](https://github.com/flwrsfralgernn/KappaBetaEngineeringProject/settings/pages): under **Build and deployment → Source**, select **GitHub Actions**. You need repository admin or maintainer access to change the publishing source. Then return to the failed Actions run and select **Re-run all jobs**. The original run can succeed after the setting is corrected; no app changes are required for this setup error.

The workflow already grants `pages: read` for the configuration lookup. It runs the typecheck and tests before that lookup so setup problems do not prevent those checks. If the setting is already correct, verify the repository's Pages availability and the workflow's access. See [GitHub's publishing-source instructions](https://docs.github.com/en/pages/getting-started-with-github-pages/configuring-a-publishing-source-for-your-github-pages-site).

Adding `enablement: true` alone does not fix setup with the default `GITHUB_TOKEN`: automatic enablement requires a different credential according to the [action's input documentation](https://github.com/actions/configure-pages/blob/v5/action.yml). Use the repository settings for this one-time setup.

## Visual audit

The tabbed redesign was checked on November 6, 2026 using the local GitHub Pages export. Connect, Settings and Live/Depth had no page or active-pane overflow at 375×667, 390×844, 768×1024 and 1440×900 after compact-layout corrections. The 320×667 maximum-depth Settings view also fit without overflow. At 844×390, the active pane scrolls while navigation remains visible. Settings persistence, the 20 bpm minimum, help dismissal/focus return, keyboard tab navigation and device-selection cancellation were checked in the browser. TypeScript, all 38 service tests and the Pages export passed. No physical ESP32 was connected for this audit. The updated automated fake-BLE browser script was syntax-checked but not executed in this session.

Local screenshots and details: [tabbed audit](audit/TABBED.md). They are generated review artifacts and are ignored by Git.

The older [instrument audit](audit/INSTRUMENT.md), [ocean redesign audit](audit/REDESIGN.md), and [original prototype audit](audit/REPORT.md) are historical references. Their screenshots and development-only demo fixtures do not document the new tabbed web interface. Physical phones and native builds remain unverified; browser layout checks do not establish hardware sensor or Bluetooth reliability.

## Implementation map

| Location | Purpose |
| --- | --- |
| `App.web.tsx`, `src/DeviceApp.tsx` | Web entry, Connect/Settings/Live tabs, shared real connection, baseline request, local settings, live sensor views and help dialogs |
| `src/device-app.css` | Responsive full-page layouts, large typography, square controls, tab placement, motion and focus states |
| `src/services/liveConnection.ts`, `src/services/telemetryProtocol.ts` | Web Bluetooth lifecycle, freshness, and versioned firmware packet decoding |
| `src/services/historyProtocol.ts`, `historyTransfer.ts`, `historyStore.ts`, `historyExport.ts` | Versioned history transfer, durable local checkpoints, bounded persistence/export |
| `src/components/HistoryView.web.tsx`, `HistoryView.css` | Paginated saved sessions, transfer state, gap counts, CSV export and storage details |
| `src/fonts.web.ts`, `public/fonts/`, `assets/fonts/` | Base-path-aware font loading, declarations, served copies, originals, and licenses |
| `app.config.js`, `scripts/build-pages.mjs`, `scripts/preview-pages.mjs` | Deployment base path, GitHub Pages export and local preview |
| `assets/ocean-background.png`, `assets/README.md` | Generated ocean image and provenance |
| `App.tsx`, `src/App.tsx`, legacy slider/picker/mock components | Previous React Native fallback UI, separate from the redesigned web entry |

The web redesign uses browser controls and Web Bluetooth. Earlier [specification](SPECIFICATION.md), [user flow](USER_FLOW.md) and Figma artifacts may describe previous designs; they should be reconciled before creating the next low-fi and hi-fi layouts.
