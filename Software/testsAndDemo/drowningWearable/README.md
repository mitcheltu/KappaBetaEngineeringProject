# DrowningWearable Arduino sketch

Open **DrowningWearable.ino** in Arduino IDE. This folder contains all sketch
tabs and required headers, including `Logging.ino` and `Diagnostics.h`. Keep the folder named
`DrowningWearable` and keep these files together when copying the sketch.

## Setup and upload

1. In Boards Manager, install **esp32 by Espressif Systems 3.3.7**.
2. In Library Manager, install the following libraries:

   | Library | Version |
   |---|---|
   | BlueRobotics MS5837 Library | 1.1.1 |
   | SparkFun BMI270 Arduino Library | 1.0.3 |
   | SparkFun MAX3010x Pulse and Proximity Sensor Library | 1.1.2 |
   | NimBLE-Arduino | 2.5.2 |

3. Select **ESP32S3 Dev Module** with these settings for the
   ESP32-S3-DevKitC-1-N8R8:

   | Setting | Value |
   |---|---|
   | Flash Size | 8 MB |
   | Partition Scheme | Default 8MB |
   | PSRAM | OPI PSRAM |
   | USB CDC On Boot | Enabled |

4. Connect the board, select its port, and click **Verify**, then **Upload**.
5. Open Serial Monitor at **115200 baud**, with **Newline** enabled, for diagnostics.

`libraries.txt` lists the same pinned libraries for Wokwi; Arduino IDE libraries
are installed through Library Manager. All `.ino` tabs compile together, so
upload the primary sketch once rather than uploading individual tabs.

See the [wiring and firmware guide](../../Electrical/README.md) for pin
assignments, power setup, and manual pump-test behavior. The
[history protocol](../HISTORY_PROTOCOL.md) describes the BLE history service.
The [build helper](../build-firmware.ps1) compiles this same folder from the
command line.

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

When updating Wokwi, copy **all** `.ino` and `.h` files from this folder, including
the new logging files, plus `libraries.txt`. Keep `diagram.json`. If your project
uses `sketch.ino` as its main tab, put `DrowningWearable.ino`'s contents there and
omit the separate `DrowningWearable.ino` tab to avoid duplicate definitions.
