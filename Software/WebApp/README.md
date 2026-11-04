# Kappa Beta Wearable Demo

A responsive wearable instrument UI built with React Native, React Native Web, TypeScript, and Expo. Adjust resting heart rate and estimated depth, tap the ocean depth scale, and explore a simulated Bluetooth connection to an ESP32 wearable.

The controls and connection dock at the top remain a simulation. The separate **ESP32 / Live sensor readings** panel connects to the Rev F firmware through Web Bluetooth and displays real depth, pressure, temperature, IMU, PPG, and device status. It does not run drowning detection or activate the valve.

## Connect the Rev F wearable

1. Build and flash `../DrowningWearable.ino` using the wiring checks and setup in the [firmware README](../README.md). Power the ESP32 and sensors.
2. Open the webapp on its HTTPS GitHub Pages address, or `http://localhost:8081`, in Chrome on Windows, macOS, or Android. Web Bluetooth requires a secure context and a direct user gesture; a plain HTTP LAN address is not sufficient. Safari/iPhone and browsers without Web Bluetooth are not supported by this connection path. See [Chrome's Web Bluetooth documentation](https://developer.chrome.com/docs/capabilities/bluetooth).
3. In the live panel, select **Connect wearable**, then choose **KappaBeta-RevF** in the browser's device picker. The upper **Connect** button is still the explicitly labelled demo connection.
4. With the pressure sensor at the water surface or in air, select **Zero depth at surface**. Depth remains unknown until firmware captures a valid session baseline. Do not zero while the sensor is submerged below the intended surface reference.
5. Read the live sensor cards. Unknown, invalid, uncalibrated, or stale values show `—`; a stream becomes stale after three seconds without a new packet. Disconnecting clears all readings. Heart rate and SpO₂ require a valid optical signal and remain experimental sensor estimates.

The page receives measurements directly from the device; no server or Wi-Fi credentials are needed. This path does not establish reliable underwater radio communication. The only writable BLE command is surface pressure calibration, never valve activation. Firmware controls valve availability and reports its state.

For the manual bench sequence, the physical **SOS** button starts a 10-second vibration countdown before opening the valve. Vibration stops when the valve opens, and the valve stays commanded open until **Cancel**. Physical Cancel aborts the countdown, stops vibration, and commands the valve closed. Low or invalid battery voltage also aborts the outputs. The live panel displays the countdown, motor state, and **Valve open until Cancel** or idle state. These are **GPIO command states, not water-flow feedback**. The webapp cannot start, cancel, or operate this sequence; use the physical controls or the firmware's Serial commands. There is no drowning-detection logic.

The service UUID is `e81b6200-7e57-4d8e-b6f4-889c1d4e8270`. Version 1 telemetry uses four 20-byte little-endian characteristics ending in `6210` (depth), `6211` (IMU), `6212` (PPG), and `6213` (system), with the same UUID suffix. Each carries a version byte, validity flags, sequence, and device uptime; payload decoding is in `src/services/telemetryProtocol.ts`. Surface calibration writes byte `1` to `e81b6220-7e57-4d8e-b6f4-889c1d4e8270`. Unsupported packet versions and malformed lengths are reported instead of interpreted as readings.

System flags include bit 5 for the vibration command and bit 6 for an active countdown. The 16-bit value at offset 10 reports countdown milliseconds while bit 6 is set; it is zero while idle or while the valve is held open until Cancel. Earlier packets with bits 5 and 6 clear still decode normally.

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
- **Reload:** Start a fresh demo with initial values, normal discovery, and no connection. Nothing is persisted.

The example ranges and presets are UI fixtures, not activation thresholds or physiological guidance. Closing or canceling the picker cancels pending mock work.

## Checks and production export

```powershell
npm test
npm run typecheck
npm run build
```

Tests cover the mock connection service plus the live packet decoder and Bluetooth transport using a fake GATT device: byte order and units, missing data, malformed packets, stale streams, cancellation, disconnect/reconnect, and calibration commands. `npm run build` exports to `dist/` for hosting at `/` by default. `dist/` is generated and ignored by Git. These checks do not replace an on-device sensor and Bluetooth test.

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
| `src/theme.ts`, `src/web.css` | Paper/navy palette, square controls, and responsive layout |
| `src/fonts.web.ts`, `public/fonts/`, `assets/fonts/` | Base-path-aware font loading, declarations, served copies, originals, and licenses |
| `app.config.js`, `scripts/build-pages.mjs` | Deployment base path and local GitHub Pages export command |
| `src/auditFixture.web.ts` | Development-only screenshot stress fixtures |
| `assets/ocean-background.png`, `assets/README.md` | Generated ocean image and provenance |

The `.web.tsx` components provide browser behavior; matching base files provide React Native fallbacks. The [specification](SPECIFICATION.md) and [user flow](USER_FLOW.md) describe the current experience.
