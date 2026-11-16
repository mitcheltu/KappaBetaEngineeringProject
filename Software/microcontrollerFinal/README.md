# microcontrollerFinal firmware

The firmware uses the Arduino framework and the Espressif TensorFlow Lite
Micro component. Build it through the ESP-IDF project in `imuDeployCNN`; the
ordinary Arduino IDE/CLI build does not resolve the TFLite Micro component.

## Build and upload

Use ESP-IDF **v5.5.2**. The project manifest pins Arduino-ESP32 **3.3.7** and
Espressif `esp-tflite-micro` **1.4.1**. ESP-IDF Component Manager downloads
those components and the TFLM dependencies during project configuration.

Install the Arduino libraries listed in `libraries.txt`. The build reads them
from `~/Documents/Arduino/libraries` by default. Set `ARDUINO_LIBRARIES_DIR`
if they are installed elsewhere; the CMake component checks the library
versions it compiles.

From the repository root:

```sh
cd Software/microcontrollerFinal/imuDeployCNN
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
idf.py -p <PORT> flash monitor
```

The project defaults target the ESP32-S3-DevKitC-1-N8R8: 8 MB flash, OPI PSRAM,
the 8 MB partition table and USB CDC on boot. All firmware `.ino` tabs are
compiled into this IDF application. See
[imuDeployCNN/README.md](imuDeployCNN/README.md) for the inference runtime
and input contract.

See the [wiring and firmware guide](../../Electrical/README.md) for pin
assignments, power setup, and manual pump-test behavior. The
[history protocol](../HISTORY_PROTOCOL.md) describes the BLE history service.

## Final IMU pipeline

The BMI270 is represented by the `ImuSensor imu` object. The final firmware
acquisition task polls the sensor every 50 ms (20 Hz) and stores timestamped
samples in a rolling buffer. A failed read occupies an invalid slot instead of
being skipped. The separate processing task copies the latest 60 samples (3
seconds) into `ImuRisk::Window` in channel order `[ax, ay, az, gx, gy, gz]`,
then releases the buffer lock before evaluation. It evaluates only a complete,
valid window. Timestamped readings and window-based risk results carry
monotonic microsecond times; the legacy millisecond timestamp remains for
existing telemetry. The CNN threshold comes from the model deployment bundle's
`kImuGateThreshold` (currently 0.9981237054).

At startup, the evaluator initializes TensorFlow Lite Micro with the model's
float32 FlatBuffer and the first reserved PSRAM workspace as its tensor arena.
It registers the model's required built-in operators, checks the `[1, 60, 6]`
input and `[1, 1]` output, then applies the exported per-axis normalization
before each invocation. The training bundle does not document sensor units;
the firmware currently passes its BMI270 values in g and degrees/second as the
model's `raw_value`, without unit conversion.

The current BMI270 hardware configuration remains 100 Hz while firmware
sampling is 20 Hz. This first integration uses the latest sensor register value
at each 50 ms tick; a future data-ready/FIFO implementation can replace that
polling without changing the window or evaluator interfaces.

The IMU object supports stationary gyro calibration. Hold the device still and
send `calibrate gyro`; send `clear calibration` to remove runtime biases.

## Continuous sensor acquisition

The `sensor-acquisition` task initializes the devices and owns the shared I²C
bus. It services the PPG FIFO every 5 ms, advances depth conversion without
waiting, and samples the IMU every 50 ms. Its timestamped
rolling buffers hold 120 IMU samples, 32 depth samples and 200 PPG readings.
The processing task and telemetry copy from these buffers under a short mutex;
they do not hold it while evaluating a window. The regular Arduino loop handles
serial commands, telemetry, history transfer and diagnostics.

Before CNN inference, each 60-sample IMU window is checked for per-sample
spacing and total span; windows with missed or excessively delayed samples are
skipped. HR/SpO2 calculation runs in the processing task from a copied
100-reading PPG window, leaving the acquisition task to service the FIFO and
other sensors. Its timing impact still needs measurement on the ESP32 hardware.

Gyro calibration commands are queued for the acquisition task. Calibration
samples are collected incrementally so the task continues servicing the other
sensors. Sensor history records also pass through a bounded, nonblocking queue
so the history ring remains single-owner in the main loop.

## Nonblocking depth sampling

The MS5837-30BA depth path keeps the initial cadence at 5 Hz
(`DEPTH_SAMPLE_PERIOD_MS = 200`). `DepthSensor::service()` is called by the
acquisition task and advances three states: idle, waiting for the 20 ms
pressure-conversion deadline, and waiting for the 20 ms temperature-conversion
deadline. It returns immediately between conversion steps, so PPG service and
the 50 ms IMU poll continue while the MS5837 is converting.

The official BlueRobotics MS5837 v1.1.1 `read()` method does not expose its
conversion or calibration primitives. The depth module therefore schedules the
same D1/D2 commands directly, validates every I²C transfer and PROM CRC, and
uses the library's MS5837-30BA compensation formulas before publishing a
sample. Failed or partial conversions never replace the current valid sample.

The `z` command queues a fresh pressure/temperature conversion through the
same state machine. A surface baseline is changed only after that conversion
passes the validity checks. The existing Hampel and EMA preprocessing remains
unchanged; the slope regression window is now one second
(`Config::DEPTH_REGRESSION_WINDOW_MS`). The 5 Hz cadence and BLE packet formats
remain unchanged. Depth readings expose the rate input interval, and
`SinkRateEstimator::compare()` returns the associated timestamped score and
validity.

## PPG interface and initial risk settings

The MAX30102 remains configured for 100 Hz internal sampling with four-sample
averaging, yielding about 25 FIFO readings per second. HR/SpO2 analysis keeps
its 100-reading window and 25-reading advance. `PpgSensor ppgSensor` exposes
the latest red/infrared sample, its validity and timestamp, the last HR/SpO2
analysis and its validity, and the configured placeholder risk. The initial
placeholder is `Config::PPG_DUMMY_RISK_SCORE` (0.0) and is explicitly marked
as dummy by `PpgRiskResult::usedDummy`; no physiological risk mapping is
implied.

Initial fusion weights are configured by `Config::DEPTH_FUSION_WEIGHT` and
`Config::PPG_FUSION_WEIGHT` (both 0.5). The combined score is
`depth weight * depth sink-risk + PPG weight * PPG risk`; the CNN controls the
gate and is not added to that score. A combined score is valid only when the
gate is open and both weighted components have valid scores. Component
validity, fusion completeness, and fusion validity are reported separately.

## Gated aligned risk processing

While the gate is closed, the processing task evaluates each newly available
valid IMU window once. A valid CNN score at or above
`Config::CNN_SCORE_THRESHOLD` opens the gate. If that exact IMU window becomes
the opening round's CNN input, its score is reused. Invalid windows and scores
are ignored. If the TFLite Micro runtime or PSRAM arena is unavailable, the
evaluator returns an invalid score and the gate remains closed.

With the gate open, the task waits for each new accepted 100-reading PPG
window, advanced by 25 readings, and uses its endpoint as target time `T`. It
copies the latest eligible IMU, depth, and PPG inputs at or before `T`, then
publishes their scores, input intervals, validity, and completion time under
one round ID. The depth component uses the existing one-second sink-rate
estimator; the PPG component uses the explicitly marked dummy score. After two
consecutive valid negative CNN results from full rounds, the gate closes and
processing returns to CNN-only inference. Existing actuator behavior does not
consume the combined risk score.

The latest round is available through the read/notify BLE risk characteristic
(`Config::RISK_UUID`). Its 96-byte little-endian payload contains the round ID,
status flags, target/completion timestamps, each component's input interval,
and CNN, depth, PPG, and combined scores. `RESULT_FRESH` means the result is no
older than `Config::RISK_RESULT_STALE_AFTER_MS`; `NEW_RESULT` marks a round ID
change since the previous telemetry packet. `CNN_VALID`, `DEPTH_VALID`,
`PPG_VALID`, `FUSION_COMPLETE`, `FUSION_VALID`, `GATE_OPEN`, and
`PPG_DUMMY_USED` distinguish the component and fused-result states. Invalid
scores are serialized as NaN. All timestamps are microseconds since boot.

DEBUG diagnostics report CNN inference duration, PPG HR/SpO2 calculation
duration, and total risk-round duration, along with component intervals and
fusion state. These software timings are for inspection; PPG's impact on
acquisition timing still needs measurement on the ESP32 hardware.

The depth estimator currently uses configurable example profile inputs in
`FirmwareConfig.h` (75 kg, 0.070 m³ displaced volume and 0.70 m² projected
area). They are provisional placeholders, not measured or calibrated wearer
values; replace them before interpreting depth scores.

Compile for the ESP32-S3 using the command in the setup section. A successful
build cannot verify live BLE delivery or actual sensor scheduling; the 50 ms
IMU cadence and PPG acquisition impact still need hardware measurement.

## Temporary blue LED countdown

SOS now turns the onboard RGB LED **solid blue for 10 seconds** instead of
running the vibration motor. The LED then turns off and the pump runs until
Cancel or `off`. Cancel, disabled controls and the existing battery interlock
turn both outputs off. The vibration motor gate on GPIO7 stays LOW throughout.

`Config::COUNTDOWN_LED_PIN` in `FirmwareConfig.h` defaults to **GPIO48** for the
original ESP32-S3-DevKitC-1 and Wokwi. For a **v1.1** board, change it to **38**;
see the [Espressif board revisions](https://docs.espressif.com/projects/esp-faq/en/latest/hardware-related/development-board.html).
The LED uses `rgbLedWrite` with only its blue channel enabled. The existing BLE
bit 5 / `vibrationOn` field now reports this temporary countdown indicator;
packet layouts and countdown timing are unchanged.

## Serial diagnostics

Logs include uptime in milliseconds, severity and subsystem. INFO is the default;
events report button presses/releases, SOS acceptance or the exact rejection,
actuator transitions, battery validity changes, sensor initialization/failures,
BLE connections and history availability. A status snapshot every two seconds
shows the battery, buttons, outputs, sensor freshness, memory and dropped-log count.
DEBUG adds raw SOS edges, sensor values, I2C transaction details and history events.
Values marked invalid/stale must not be interpreted as current measurements.

| Newline command | Effect |
|---|---|
| `status` | Current diagnostic snapshot, including when automatic logging is off |
| `log debug` | Detailed diagnostics |
| `log info` | Events and periodic status (default) |
| `log warn` / `log error` | Restrict automatic output to warnings/errors or errors |
| `log off` | Disable automatic logs; explicit command replies still print |
| `help` | Command list |
| `sos` | Request the manual sequence; the control task reports acceptance/rejection |
| `off` | Turn off the blue LED and stop the pump |
| `z` | Take a new pressure sample for surface zero |
| `calibrate gyro` | Capture stationary gyro bias; keep the IMU motionless |
| `clear calibration` | Clear runtime IMU calibration biases |

The bounded 48-line queue uses no-wait sends. A separate lower-priority task is
the sole normal Serial writer; control/sensor/BLE callers never wait for serial
output or queue space. Full queues drop new lines and report a cumulative count.
Lines are limited to 223 bytes including newline; truncated lines end in `...`.
`log_dropped` and `truncated` in `status` reveal missing detail. Firmware and core
startup/error output can still appear independently of these log-level settings.

## Wokwi battery knob and unresponsive SOS

In project `479911826194402305`, the battery pot is wired correctly as a **sense
input**: `VCC -> 3V3`, `GND -> GND`, `SIG -> GPIO1`. It does not power the ESP32.
The firmware assumes the physical battery divider R5/R6 is 100k/100k, so it
multiplies the calibrated ADC voltage by two. The simulated pot supplies that
divider midpoint directly; no extra simulated divider is needed.

| Estimated battery | Diagnostic state | SOS / active outputs |
|---|---|---|
| Below 2500 mV | `INVALID_LOW` | Block / stop |
| 2500–3399 mV | `LOW` | Block / stop |
| 3400–4350 mV | `READY` | Allowed after SOS release |
| Above 4350 mV | `INVALID_HIGH` | Block / stop |

Therefore **turning the knob up too far also stops the test**. The accepted
calibrated GPIO1 reading is 1700–2175 mV; 1850 mV represents a 3.70 V battery.
Use the printed voltage to set the knob, since simulator ADC conversion affects
the exact knob position. The saved pot value `528` accepted SOS in the linked
project; `0` and `1023` rejected it. An abort never resumes automatically:
restore a `READY` voltage, release SOS and press it again. Cancel remains an
unconditional stop, including while the battery reading is invalid.

Example diagnostic messages (illustrative voltages):

```text
[      2500 ms] WARN  BATTERY  GPIO1=2300 mV => battery=4600 mV; INVALID_HIGH; SOS BLOCKED (allowed 3400..4350 mV)
[      2500 ms] INFO  ACTUATOR IDLE: blue_led=0 pump=0; vibration=0; reason=INVALID_HIGH
[      3000 ms] WARN  SOS      button press REJECTED: INVALID_HIGH
```

The linked diagram also has an **MPU6050**, while this firmware requires a
**BMI270**; its depth/HR/SpO2 pots and skin switch are not read by this hardware
firmware. MAX30102 and MS5837 are absent from that diagram. Sensor-init warnings
are separate from the battery interlock. Wokwi also does not simulate ESP32
Bluetooth. See the [Wokwi potentiometer reference](https://docs.wokwi.com/parts/wokwi-potentiometer)
and [ESP32 simulation features](https://docs.wokwi.com/guides/esp32#simulation-features).

When updating Wokwi, copy **all** `.ino` and `.h` files from this folder,
including the IMU window/evaluator files, plus `libraries.txt`. If your project
uses `sketch.ino` as its main tab, put `microcontrollerFinal.ino`'s contents
there and omit the separate `microcontrollerFinal.ino` tab to avoid duplicate
definitions.
