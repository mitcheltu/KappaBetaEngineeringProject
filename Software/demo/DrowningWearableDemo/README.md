# DrowningWearableDemo (Rev J)

Open **DrowningWearableDemo.ino** in Arduino IDE. Keep all files in this folder
together. This is a physical-button controller with live sensor readings,
recorded history and **scripted scenario playback with on-device inference**
for the web app. Live sensor readings never reach the models and never start
the outputs. The vibration -> pump sequence starts from physical SOS, or from
the fused model decision during a *scripted* scenario (see
[Demo scenarios](#demo-scenarios-and-on-device-inference)). Physical Cancel and an
IMU **double tap** stop it.

## Files

| File | Purpose |
|---|---|
| `DrowningWearableDemo.ino` | Startup and main sensor/telemetry loop. |
| `FirmwareConfig.h` | Rev J pins, pump PWM, sample intervals, battery limits and BLE identifiers. |
| `FirmwareShared.h` | Shared readings, atomic control snapshots and function declarations. |
| `DemoSequence.h` | Testable pulse/pump timing, cancel priority and release-to-rearm rules. |
| `Actuators.ino` | Independent task reading buttons/battery and driving GPIO7/GPIO13. |
| `PumpOutput.ino`, `PumpRamp.h` | 20 kHz pump PWM, two-second linear startup, immediate stop and fault latch. |
| `Sensors.ino` | Stock BMI270, MAX3010x and MS5837 library calls, validity and surface zero. |
| `Bluetooth.ino` | Advertising, live packets and resumable history transfer. |
| `History.ino`, `SensorHistory.h` | PSRAM allocation and acquisition-time recording. |
| `SensorTelemetry.h` | Shared live/history units, validity and freshness. |
| `TelemetryProtocol.h` | The four 20-byte live packet layouts. |
| `HistoryProtocol.h`, `HistoryBuffer.h`, `HistoryTransfer.h` | Existing v1 wire format, bounded ring and ACK/retry/snapshot handling. |
| `Logging.ino`, `SerialCommands.ino` | Queued diagnostics and Serial commands (`scenario`, `scenario <id>`, `scenario abort`). |
| `Scenario.ino` | Scenario task: START/ABORT queue, 30 s arm buffer, playback, events to Serial, status mailbox for BLE. |
| `ScenarioController.h`, `ScenarioProtocol.h`, `ScenarioSource.h` | Scenario state machine, BLE wire types, scripted-input interface. |
| `ScenarioData.h` | Generated: the four compiled-in scenarios (const flash) from `../scenarios/*.csv`. |
| `DemoInference.h` | IMU, pulse and oxygen tracks over scripted input, and the fusion rule. |
| `ImuCnn.h`, `ImuCnnParams.h` | Hand-written forward pass of the teammate's IMU CNN (`imu_cnn_bce.tflite`); no TFLite Micro needed. |
| `PulseLoss.h`, `PulseCnnV5.h`, `PulseCnnV5Params.h`, `PulseLossParams.h` | Pulse models; `cnn_v5_h` is used (copied from `ppgSoftware`). |
| `OxygenConcern.h`, `OxygenTrack.h`, `PpgWindow.h` | Oxygen `concern_level` rule (80 -> 60 %) and the IR ring. |
| `DoubleTap.h` | Double-tap detector (host-tested by `Software/tests/double_tap_test.cpp`), fed by the BMI270 FIFO at 100 Hz. |
| `libraries.txt`, `build.ps1` | Dependency versions and a compile-only helper. |

Custom sensor drivers are omitted. History uses the existing protocol's record
kinds 1–5; scenario events go to Serial and the scenario status characteristic,
not to history.

## Manual sequence

1. Release SOS and Cancel after boot. A valid battery reading is required.
2. Press SOS once. The vibration motor is ON for 1 second, then OFF for
   0.5 seconds, repeating for a **10-second elapsed window**. This produces
   seven 1-second pulses, separated by six 0.5-second pauses.
3. At the end of that window, vibration stops and the pump runs for
   **60 seconds, including a two-second startup ramp**, then stops automatically.
   GPIO13 drives DRV8871 IN1 at **20 kHz**, with 10-bit duty increasing linearly
   from 0 to 1023 over 2,000 ms, updated by the 5 ms control task. At full duty
   the installed ESP32 core produces steady HIGH with no low pulse; LEDC stays
   attached so stop can always set duty zero. Each new start ramps from zero.
   The full sequence lasts about
   70 seconds; output transitions have normal task scheduling latency.
4. Physical Cancel aborts either phase, including during a vibration pause or
   at the pump-start boundary. An aborted sequence does not resume. An IMU
   **double tap** (two jolts of at least 0.6 g, 120–500 ms apart, as set in
   `DoubleTap.h`) does the same, then gives a 150 ms confirmation buzz.
5. Release SOS and press it again to start another sequence. Holding SOS at
   boot or through completion/cancellation cannot retrigger it. Presses during
   a running sequence do not queue or extend a later sequence.

Control runs in its own 5 ms FreeRTOS task. Physical Cancel is read without
debouncing and has priority; SOS uses 30 ms debounce. No actuator phase uses a
blocking delay. The stock depth library can block sensor/BLE work for about
40 ms without making physical Cancel wait for that read. Serial `off` is an
additional stop command serviced by the main loop; physical Cancel is the
direct stop input. Only the physical SOS button starts the sequence.

The existing battery policy remains: an estimated **3.4 to 4.35 V** is required
for either output. Low/invalid battery aborts an active sequence. USB power
does not substitute for a valid battery-divider reading on GPIO1. The
3V3 source jumper JP1 must remain consistent with the board's USB/power setup;
the firmware does not control power-source selection. A PWM initialization or
write failure stops the outputs and disables starts until reboot.

## Rev J pins

Source: [KappaBetaSchematic_RevJ.pdf](../../../Electrical/KappaBetaSchematic_RevJ.pdf),
dated 2026-11-16, sheet 1/1. Startup behavior follows
[StaggerStart.png](../../../Electrical/StaggerStart.png).
These are ESP32 GPIO numbers, not header-pin positions.

| Connection | GPIO / configuration |
|---|---|
| I2C SDA / SCL | 8 / 9, 400 kHz |
| BMI270 INT1 / MAX30102 INT | **15 / 4**; Rev J swapped these, inputs currently polled |
| Vibration motor gate | **7**, active HIGH |
| Pump DRV8871 IN1 | **13**, PWM ramp then steady HIGH; IN2 wired to GND |
| Cancel / SOS | 5 / 6, normally-open switches to GND, `INPUT_PULLUP` |
| Battery divider | ADC GPIO1; 100k/100k, estimated battery = 2 x ADC voltage |

Both output commands start LOW. The blue LED is not used as the vibration
substitute in this sketch. Sensor addresses are BMI270 `0x68`, MAX30102 `0x57`,
and MS5837 `0x76`. All three sensor buses/power connections follow the 3.3 V
schematic. Rev J powers the DRV8871/pump through the TPS63020 5 V and XL6009
12 V stages. The ramp does not establish pump starting torque, peak current,
or supply stability; measure these on the assembled hardware.

## Original libraries and sampling

Install through Arduino Library Manager:

| Library | Version |
|---|---|
| SparkFun BMI270 Arduino Library | 1.0.3 |
| SparkFun MAX3010x Pulse and Proximity Sensor Library | 1.1.2 |
| BlueRobotics MS5837 Library | 1.1.1 |
| NimBLE-Arduino | 2.5.2 |

The sensor code calls these original libraries directly. There is no
`CheckedBMI270`, custom MS5837 driver/filter, or replacement MAX30102 FIFO
reader. MAX30102 setup is checked by reading configuration registers through
the original library API. Generic address-ACK checks and finite/range checks
help detect missing devices; the original library APIs do not expose every
I2C transfer failure.

| Stream | Demo setting |
|---|---|
| IMU | Nominal 20 Hz host reads; BMI270 internally runs at 100 Hz, +/-8g and +/-2000 degrees/s |
| PPG | Hardware 100 Hz with averaging of four: **25 red/IR pairs per second** from the stock FIFO |
| Depth/temperature | Nominal 5 Hz; original blocking pressure/temperature conversions |
| Web telemetry | Nominal 5 Hz snapshots of the latest valid measurements |

These are configured targets, not measured timing guarantees. This demo does
not implement the earlier proposed 20/100/20 Hz acquisition/model pipeline.
Sensor freshness uses host read-completion times. The live packet timestamp
marks the telemetry snapshot; packets do not carry every individual sensor
sample or hardware acquisition timestamp. This stream is for the live demo,
not synchronized model-training data.
The stock MAX library has only four software slots; the queue is serviced
frequently and immediately before/after depth reads. Hardware FIFO rollover
uses the stock enabled setting. The hardware overflow counter is checked
before the library reads data: equal write/read pointers can indicate a full
wrapped FIFO, which the original `check()` otherwise treats as empty. Hardware
overflow, batches of four or more pairs, or service gaps of 100 ms or more clear
both queues, invalidate raw/HR/SpO2 values, record a gap and increment the PPG
discontinuity counter. Only subsequent fresh pairs can restore validity.
Sensors that fail initialization/read checks are retried every five seconds.

HR/SpO2 use the library's Maxim algorithm: the first estimate needs about four
seconds of uninterrupted usable signal, then updates about once per second.
The optical signal gate is an experimental validity check, not a detection
model. Raw red/IR can still be displayed when derived values are invalid.
Hold the optical sensor steadily against skin for testing. A failed/gapped
window shows unavailable estimates instead of reusing them indefinitely.

If raw red/IR are missing, send `log debug` and then `ppg` in Serial Monitor.
The same register dump is emitted automatically before a no-samples timeout
reinitializes the sensor. `PPG_REG` should report `part=15 mode=03 spo2=27`,
`fifo=50` (or `5F` with the module's almost-full threshold), and
`LED=60/60 config_ok=1`. `PPG_FIFO` reports hardware write/read pointers,
overflow, interrupt status and the total number of received pairs. These
diagnostics do not consume optical samples; reading interrupt status clears
its flags (the demo uses polling). A successful chip-ID check alone is no
longer considered successful configuration.

`config_ok=0` indicates configuration drift, failed setup, or failed readback;
the stock API returns zero for a failed register read. `config_ok=1` with a
sample count that stays zero and stationary FIFO pointers requires checking
`OVF` too: a nonzero value counts lost hardware samples, not an empty FIFO.
The old rollover-disabled demo missed this condition and timed out with
`samples=0`; the revised poller explicitly recovers it. With `OVF=0`, investigate
sample production/FIFO access rather than skin contact or HR/SpO2 calculation.
Do not treat the last stored `red=0 IR=0` as an actual measurement while raw
validity is false. Capture both diagnostic lines before changing wiring or
sensor settings.

Register semantics and rollover behavior are specified in the
[MAX30102 datasheet, FIFO sections](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX30102.pdf).

Depth remains uncalibrated until **Zero depth** in the app or Serial `z` is
used at the water surface. Losing/reinitializing the depth sensor clears its
baseline and requires another surface zero.

## Arduino build and upload

Use **ESP32S3 Dev Module**. The installed verification toolchain is esp32
core **3.3.12**. Set:

| Setting | Value |
|---|---|
| USB CDC On Boot | **Enabled** (required for USB Serial commands/logs) |
| USB Mode | Hardware CDC and JTAG |
| Flash Size | Match the board: 8 MB in the Rev J N8R8 drawing, or 16 MB on a 16 MB unit |
| Partition Scheme | Default 8MB for 8 MB; 16M Flash (2MB APP/12.5MB FATFS) for 16 MB |
| PSRAM | **OPI**, required for history on the N8R8 board |

Select the board's current USB port and upload this sketch once. Serial Monitor
uses **115200 baud and Newline**. If only the ROM banner appears after upload,
press RESET/EN once with BOOT released, then reselect the port if necessary.
The older `Software/build-firmware.ps1`
still selects the original `DrowningWearable`; use this folder's helper:

```powershell
./Software/testsAndDemo/DrowningWearableDemo/build.ps1
# For a board with 16 MB flash:
./Software/testsAndDemo/DrowningWearableDemo/build.ps1 -FlashSize 16M
```

The helper only compiles and exports artifacts under `tmp/demo-revi/bin`.
It never uploads firmware, installs dependencies, or changes IDE settings.
It uses two compiler workers and normal Arduino caching by default.
Completed artifacts are copied from the CLI cache to the selected output
directory; the helper does not run the core's duplicate `sketch/build` export.

The original live-only build was verified on 2026-11-09 with core 3.3.12 and
the listed original libraries. Rebuild this revision with the helper above.
The stock MAX3010x headers emit buffer-macro and unused-array warnings; its
library sources also emit existing unused-variable/parentheses warnings on
a cold build. The build succeeds without modifying those libraries.

## Web app

Connect to **KappaBetaDemo** using the current web app. The same BLE service
and 20-byte telemetry layout are retained. It streams depth, motion, red/IR,
available HR/SpO2, battery, buttons, vibration command and pump command.

History starts at boot, including while disconnected. A **4 MiB PSRAM ring**
holds 131,072 records (about **39.7 minutes** at the nominal 20 IMU + 25 averaged
PPG + 5 depth + 5 system records/s, excluding extra calibration/gap events).
The ring falls back to 2 or 1 MiB, preserving 512 KiB free PSRAM at allocation.
No ML memory is reserved. If allocation fails, the app explicitly shows
**Live only** and continues streaming without claiming to save history.

Every consumed 25 Hz averaged red/IR pair is recorded, including poor-contact
signals with HR/SpO2 invalid. PPG discontinuities have an invalid marker; FIFO
times are estimated/backdated and flagged. IMU and depth use read-completion
times; system status is sampled at 5 Hz. Surface-zero success, failure and
baseline loss are recorded. Records use 64-bit device microseconds, not wall
clock time; different sensor kinds can have out-of-order timestamps.

On connection the app imports a finite snapshot, verifies CRCs and saves to
IndexedDB before acknowledging blocks, then enables live readings and polls
for new history. Recording continues throughout. Reconnecting during the same
boot resumes from the browser's saved cursor. Overwritten records become
explicit gaps. Reset/power loss clears device history and starts a new boot
session; records already saved in the browser remain there. Export important
sessions using **Live → History → Export CSV**.

Profile settings remain optional/unsupported in this sketch and do not block
live/history connection. The app has no pump-control command. Its only output-related
ability is the demo-scenario ARM described below.

System flags bit 7 identifies a physical vibration motor; bit 5 reports its
actual ON/OFF command during each pulse. The updated web app labels this
correctly while keeping the blue-LED label for older firmware. The existing
remaining-time field reports vibration time or the timed pump's remaining
seconds. Older deployed web app copies can still receive the packets but may
show the old blue-LED label and omit the pump timer until updated.

The app cannot start, stop or time the outputs directly. **Demo-only exception:** it can
ARM a compiled-in scripted scenario, and the device's own models may then start the
sequence. Use physical SOS/Cancel or a double tap; Serial `off` is also available.
App settings do not change this demo's sequence.

## Demo scenarios and on-device inference

A scenario replays scripted data stored in flash through the same models used in the
research notebooks. Live sensors are never inputs:

| Track | Model / rule | Cadence | Alert |
|---|---|---|---|
| IMU | Teammate's IMU CNN (`Software/models/swim_imu_esp32_deployment`), 3 s at 20 Hz | every 1 s | Gate opens at score >= 0.998, closes after 2 lower scores (same as `microcontrollerFinal`'s `CnnRiskGate`) |
| Pulse | Notebook 04 v5 `cnn_v5_h`, 16 s at 100 Hz | every 2 s | Score >= 0.6635 (high-specificity point), latched |
| Oxygen | Notebook 03 `concern_level`, 80 -> 60 % | every 1 s | Concern >= 0.5 for 3 s, latched |

**Fusion (team decision):** the sequence is requested once, when the **IMU gate is open AND
(pulse OR oxygen) has alerted**. Depth is not used because no dataset supplies it.
`DemoSequence` accepts the request only for the run that is playing now, only while idle,
and only with the same battery and enable interlocks as SOS. Cancel, double tap or
Serial `off` stop it and abort the run. The app can ABORT only before actuation.

Flow: START (app or Serial `scenario <id>`) -> **ARMED for 30 s**, to get the wearable
into the water -> **PLAYING** 110 s of scripted data -> **ALERTED** -> **ACTUATING**:
10 s pulsed vibration, then 60 s pump -> **DONE**. Expected outcomes, data sources and
licence notes are in [../scenarios/README.md](../scenarios/README.md).

BLE (service `e81b6200-...`, little-endian, protocol v1, `ScenarioProtocol.h`):

| Characteristic | UUID | Payload |
|---|---|---|
| Catalog | `e81b6240-7e57-4d8e-b6f4-889c1d4e8270` (read) | `{version u8, count u8, catalogCrc u16}` then `count` x 40-byte `{id u8, version u8, imuSamples u16, irSamples u32, spo2Samples u32, contentCrc32 u32, name char[24]}` |
| Command | `e81b6241-...` (write, 8 B) | `{opcode u8 (1 START, 2 ABORT), version u8, token u16 (nonzero), scenarioId u8, reserved u8 = 0, catalogCrc u16}` |
| Status | `e81b6242-...` (read/notify, 20 B) | `{type u8 = 1, version u8, state u8, reason u8, commandToken u16, scenarioId u8, flags u8, remainingMs u32, catalogCrc u16, pulseScore u16 (x1e-4, 0xFFFF n/a), oxygenConcern u16, runToken u16}`; flags: 1 pulse alert, 2 oxygen alert, 4 trigger accepted, 8 pulse valid, 16 concern valid, 32 scenarios compiled, 64 playback finished, 128 IMU gate open |

States: 0 idle, 1 armed, 2 playing, 3 alerted, 4 actuating, 5 done, 6 aborted, 7 rejected.
`catalogCrc` is the low 16 bits of CRC-32 over all 16-byte descriptors. A START with a
stale CRC is refused. Status notifies on every change and at least once per second.
`Software/tests/scenario-protocol-fixture.json` (from `scenario_fixture.cpp`) ties these
bytes to the web app's tests.

Serial: `scenario` lists the catalog and status; `scenario 2` arms #2; `scenario abort`
aborts before actuation. `log debug` adds per-second IMU and oxygen lines; pulse results,
fused alerts and actuation are logged at INFO.

The models run in a priority-1 task on core 0 with about 110 KB of static workspace. The
pulse CNN's ESP32-S3 time is reported in each pulse log line, but it has not been measured
on hardware yet. Double tap reads every 100 Hz sample from the BMI270 FIFO, so the 40 ms
depth read cannot hide taps. A strong swimming jolt could cancel; raise
`DoubleTap::TAP_G` if that happens.

## Diagnostics and verification

Commands: `help`, `status`, `ppg` (register dump at INFO/DEBUG), `off`, `z`,
`scenario`, `scenario <id>`, `scenario abort`, and
`log error`, `log warn`, `log info`, `log debug`, `log off`.
Logging uses a bounded queue and a separate writer task, so disconnected or
slow Serial does not block the actuator task. INFO status is printed every
two seconds; `status` still replies when automatic logging is off.

The sequence host test checks all pulse boundaries, the 10/60-second phase
deadlines, cancellation, battery/enable interlocks, rearming, delayed
scheduling, and `millis()` rollover:

```powershell
$env:PATH = 'C:/msys64/ucrt64/bin;' + $env:PATH
New-Item -ItemType Directory -Force tmp/demo-revi | Out-Null
g++ -std=c++17 -Wall -Wextra -Werror -I Software/testsAndDemo/DrowningWearableDemo Software/tests/demo_sequence_test.cpp -o tmp/demo-revi/demo-sequence-test.exe
./tmp/demo-revi/demo-sequence-test.exe
```

Run the shared history test with this demo's include directory to check
snapshots, retries, overwrite gaps, polling, reconnect and no-memory fallback.
`pump_ramp_test.cpp` checks the Rev J pins, every millisecond of the ramp,
restart, timer rollover, cancellation/interlocks and the 60-second deadline.
`demo_sensor_history_test.cpp` checks the shared live/history encoders and
generates `../tests/demo-sensor-history-fixture.json`; the web tests decode its
actual packets, default-MTU fragments and invalid readings. See
[host test commands](../tests/README.md#rev-j-demo-live-history-and-pump-startup).
`../../tests/demo_stock_ppg/README.md` describes the test against the
unchanged stock MAX3010x FIFO implementation. Tests do not establish wiring, actual pump flow, sensor accuracy,
or physical cancellation latency. Rehearse vibration/cancel, pump/cancel,
automatic pump stop, sensor zero/contact, and BLE reconnect on the assembled
hardware before presenting it.

Bench acceptance for this revision: verify GPIO13 is LOW at boot; observe
20 kHz and the two-second ramp on a scope, then steady HIGH; cancel during
the ramp and at full power; restart and confirm a fresh ramp. Confirm pump
startup/current and supply rails under the actual load. Record sensors while
disconnected, connect/import, zero depth, check live cards and exported CSV,
disconnect/reconnect without reset, then reset and check a new session.
Software tests and compilation do not replace this hardware/BLE check.

Scenario/inference build, verified 2026-11-16 (core 3.3.12, same libraries): compile/link
uses **1,340,798 bytes flash (40 %) / 163,744 bytes static RAM (49 %)**. Host suites:
`scenario_data_test` (all four scenarios through the real controller, IMU CNN parity),
`scenario_inference_test`, `scenario_controller_test`, `double_tap_test`,
`demo_sequence_test`; web: 67 tests including the scenario protocol fixture. Not tested on
hardware: BLE scenario flow, FIFO double tap, CNN timing, outputs.

```powershell
foreach ($t in 'scenario_data_test','scenario_inference_test','scenario_controller_test','double_tap_test','demo_sequence_test') {
  g++ -std=c++17 -O2 -Wall -Wextra -Werror -I Software/testsAndDemo/DrowningWearableDemo "Software/tests/$t.cpp" -o "tmp/demo-scenario/$t.exe"; & "./tmp/demo-scenario/$t.exe"
}
```

Earlier live/history build, verified 2026-11-16 with ESP32 core 3.3.12 and the pinned libraries: compile/link
uses **638,766 bytes flash / 43,456 bytes static RAM** (the recording ring is a
separate runtime PSRAM allocation). The five host suites (sensor history, shared
history transfer, pump ramp, manual sequence and stock PPG FIFO), all **60** web
tests, TypeScript and the Pages export passed. Real Chromium checks passed for
IndexedDB rollback/durable checkpoints and 5,001-entry CSV export, plus desktop
and mobile fake-BLE history import, live readings, zeroing, polling and reconnect.
No upload or physical hardware/BLE test was performed.
