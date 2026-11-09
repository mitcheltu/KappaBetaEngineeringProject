# DrowningWearableDemo (Rev I)

Open **DrowningWearableDemo.ino** in Arduino IDE. Keep all files in this folder
together. This is a physical-button demonstration and live sensor streamer.
It contains no drowning detection, trained model, sensor-fusion trigger, or
automatic pump activation from sensor readings.

## Files

| File | Purpose |
|---|---|
| `DrowningWearableDemo.ino` | Startup and main sensor/telemetry loop. |
| `FirmwareConfig.h` | Rev I pins, sample intervals, battery limits and BLE identifiers. |
| `FirmwareShared.h` | Shared readings, atomic control snapshots and function declarations. |
| `DemoSequence.h` | Testable pulse/pump timing, cancel priority and release-to-rearm rules. |
| `Actuators.ino` | Independent task reading buttons/battery and driving GPIO7/GPIO13. |
| `Sensors.ino` | Stock BMI270, MAX3010x and MS5837 library calls, validity and surface zero. |
| `Bluetooth.ino` | Advertising, live packets and the app's required live-only handshake. |
| `TelemetryProtocol.h` | The four 20-byte live packet layouts. |
| `HistoryProtocol.h`, `HistoryBuffer.h`, `HistoryTransfer.h` | Existing tested handshake compatibility; the buffer stays empty. |
| `Logging.ino`, `SerialCommands.ino` | Queued diagnostics and Serial commands. |
| `libraries.txt`, `build.ps1` | Dependency versions and a compile-only helper. |

Detection, ML buffers, history recording and custom sensor drivers are omitted.
The reused history handshake retains about 2 KB of static transfer staging,
but allocates no recording ring or PSRAM. Keeping it avoids a new BLE protocol
and preserves the current app's connection/reconnect behavior.

## Manual sequence

1. Release SOS and Cancel after boot. A valid battery reading is required.
2. Press SOS once. The vibration motor is ON for 1 second, then OFF for
   0.5 seconds, repeating for a **10-second elapsed window**. This produces
   seven 1-second pulses, separated by six 0.5-second pauses.
3. At the end of that window, vibration stops and the pump turns on for
   **60 seconds**, then stops automatically. The full sequence lasts about
   70 seconds; output transitions have normal task scheduling latency.
4. Physical Cancel aborts either phase, including during a vibration pause or
   at the pump-start boundary. An aborted sequence does not resume.
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
Rev I circuit's JP1 must be removed when USB is plugged in, as annotated in
the schematic; this firmware does not change that power wiring.

## Rev I pins

Source: `KappaBetaSchematic_RevI.pdf`, drawing dated 2026-11-09, sheet 1/1.
These are ESP32 GPIO numbers, not header-pin positions.

| Connection | GPIO / configuration |
|---|---|
| I2C SDA / SCL | 8 / 9, 400 kHz |
| BMI270 INT1 / MAX30102 INT | 4 / 15; inputs, currently polled |
| Vibration motor gate | **7**, active HIGH |
| Pump gate | **13**, active HIGH; Rev I changed this from the older GPIO16 |
| Cancel / SOS | 5 / 6, normally-open switches to GND, `INPUT_PULLUP` |
| Battery divider | ADC GPIO1; 100k/100k, estimated battery = 2 x ADC voltage |

Both output gates start LOW. The blue LED is not used as the vibration
substitute in this sketch. Sensor addresses are BMI270 `0x68`, MAX30102 `0x57`,
and MS5837 `0x76`. All three sensor buses/power connections follow the 3.3 V
schematic. Pump power and its MOSFET driver remain external hardware.

## Original libraries and sampling

Install through Arduino Library Manager:

| Library | Version |
|---|---|
| SparkFun BMI270 Arduino Library | 1.0.3 |
| SparkFun MAX3010x Pulse and Proximity Sensor Library | 1.1.2 |
| BlueRobotics MS5837 Library | 1.1.1 |
| NimBLE-Arduino | 2.5.2 |

The sensor code calls these original libraries directly. There is no
`CheckedBMI270`, custom MS5837 driver/filter, or custom MAX30102 register/FIFO
reader. Generic address-ACK checks and finite/range checks help detect missing
devices; the original library APIs do not expose every I2C transfer failure.

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
frequently and immediately before/after depth reads. Batches of four or more
pairs or service gaps of 100 ms or more discard the HR/SpO2 window. Sensors
that fail initialization/read checks are retried every five seconds.

HR/SpO2 use the library's Maxim algorithm: the first estimate needs about four
seconds of uninterrupted usable signal, then updates about once per second.
The optical signal gate is an experimental validity check, not a detection
model. Raw red/IR can still be displayed when derived values are invalid.
Hold the optical sensor steadily against skin for testing. A failed/gapped
window shows unavailable estimates instead of reusing them indefinitely.

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
| Flash Size | Match the board: 8 MB in the Rev I N8R8 drawing, or 16 MB on a 16 MB unit |
| Partition Scheme | Default 8MB for 8 MB; 16M Flash (2MB APP/12.5MB FATFS) for 16 MB |
| PSRAM | OPI for an R8 board; the demo itself does not allocate PSRAM history |

Select the board's current USB port and upload this sketch once. Serial Monitor
uses **115200 baud and Newline**. The older `Software/build-firmware.ps1`
still selects the original `DrowningWearable`; use this folder's helper:

```powershell
./Software/testsAndDemo/DrowningWearableDemo/build.ps1
# For a board with 16 MB flash:
./Software/testsAndDemo/DrowningWearableDemo/build.ps1 -FlashSize 16M
```

The helper only compiles and exports artifacts under `tmp/demo-revi/bin`.
It never uploads firmware, installs dependencies, or changes IDE settings.
It uses two compiler workers and normal Arduino caching by default.

Verified on 2026-11-09 with core 3.3.12, the 8 MB settings above, and the
listed original libraries: **628,171 bytes flash / 43,256 bytes static RAM**.
The stock MAX3010x headers emit buffer-macro and unused-array warnings; its
library sources also emit existing unused-variable/parentheses warnings on
a cold build. The build succeeds without modifying those libraries.

## Web app

Connect to **KappaBetaDemo** using the current web app. The same BLE service
and 20-byte telemetry layout are retained. The demo implements the required
history handshake with **zero recording capacity**, so the app enters
**Live only** mode. It streams depth, motion, red/IR, available HR/SpO2,
battery, buttons, vibration command and pump command. It records no sensor
history and reserves no ML workspaces.

System flags bit 7 identifies a physical vibration motor; bit 5 reports its
actual ON/OFF command during each pulse. The updated web app labels this
correctly while keeping the blue-LED label for older firmware. The existing
remaining-time field reports vibration time or the timed pump's remaining
seconds. Older deployed web app copies can still receive the packets but may
show the old blue-LED label and omit the pump timer until updated.

The app cannot start or cancel the outputs. Use physical SOS/Cancel; Serial
`off` is also available. App settings do not change this demo's sequence.

## Diagnostics and verification

Commands: `help`, `status`, `off`, `z`, and
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

Run the shared history test with this demo's include directory to check the
empty-history handshake. Web tests include the motor marker and 60-second
pump timer. `../../tests/demo_stock_ppg/README.md` describes the test against the
unchanged stock MAX3010x FIFO implementation. Tests do not establish wiring, actual pump flow, sensor accuracy,
or physical cancellation latency. Rehearse vibration/cancel, pump/cancel,
automatic pump stop, sensor zero/contact, and BLE reconnect on the assembled
hardware before presenting it.

Validation completed: full ESP32-S3 compile/link/export, the sequence and
stock-FIFO host tests, shared history host tests against this folder's
headers, all 45 web tests, and TypeScript checking. No upload or physical
hardware test was performed.
