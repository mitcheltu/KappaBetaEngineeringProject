# Electronics Components

## Current firmware pin assumptions: Rev F with pump substitution

The selected actuator is the [CrocSee B09XH1GYYQ diaphragm pump](https://www.amazon.com/dp/B09XH1GYYQ). It replaces the solenoid on **GPIO16**. The original Rev F net name `SOL_EN` now means pump enable; `MOTOR_EN` on **GPIO7 remains the vibration motor**. This is a user-directed substitution, not an updated schematic revision or proof that the power stage has been validated.

The firmware targets **ESP32-S3-DevKitC-1-N8R8**. Numbers below are GPIO numbers; the separate header positions are those shown in Rev F. Verify the actual board's labels and breakout pin order when assembling.

| GPIO | Rev F header | Connection assumed | Firmware mode and polarity |
|---:|---|---|---|
| 8 | J1-12 | BMI270 SDA, MS5837 SDA and MAX30102 SDA | Shared I2C SDA, external pull-ups to 3.3 V |
| 9 | J1-15 | BMI270 SCL, MS5837 SCL and MAX30102 SCL | Shared I2C SCL, external pull-ups to 3.3 V |
| 4 | J1-4 | BMI270 INT1 | Input; wired but acquisition currently polls I2C |
| 15 | J1-8 | MAX30102 INT | Input; wired but FIFO acquisition currently polls I2C |
| 5 | J1-5 | Cancel normally-open reed, other end to GND | INPUT_PULLUP; LOW means pressed; immediately stops both outputs |
| 6 | J1-6 | SOS normally-open reed, other end to GND | INPUT_PULLUP; LOW means pressed; debounced 30 ms |
| 7 | J1-7 | MOTOR_EN -> R1 (100 ohms) -> Q1 gate | Vibration output; HIGH on, LOW off |
| 16 | J1-9 | Former SOL_EN -> R3 (100 ohms) -> Q2 gate | Pump output; HIGH on, LOW off; steady on/off, no PWM |
| 1 | J3-4 | VBAT_SENSE, midpoint of R5/R6 (100k/100k) | ADC1 input, 12-bit, 11 dB attenuation; battery estimate = 2 x calibrated ADC voltage |

The authoritative constants are in [FirmwareConfig.h](../Software/DrowningWearable/FirmwareConfig.h); output initialization is in [DrowningWearable.ino](../Software/DrowningWearable/DrowningWearable.ino) and actuator control is in [Actuators.ino](../Software/DrowningWearable/Actuators.ino).

### Arduino setup: official BlueRobotics library

Install **esp32 by Espressif Systems 3.3.7** through Boards Manager. Install these library versions through Arduino Library Manager:

| Library | Author | Version |
|---|---|---|
| BlueRobotics MS5837 Library | BlueRobotics | 1.1.1 |
| SparkFun BMI270 Arduino Library | SparkFun Electronics | 1.0.3 |
| SparkFun MAX3010x Pulse and Proximity Sensor Library | SparkFun Electronics | 1.1.2 |
| NimBLE-Arduino | h2zero | 2.5.2 |

Choose the exact **BlueRobotics MS5837 Library** entry, using `#include <MS5837.h>`; no custom KappaBeta library or custom ZIP is needed. Other libraries named MS5837 can expose a different API. [Official library metadata](https://raw.githubusercontent.com/bluerobotics/BlueRobotics_MS5837_Library/1.1.1/library.properties)

For Arduino IDE, open [Software/DrowningWearable/DrowningWearable.ino](../Software/DrowningWearable/DrowningWearable.ino) directly. The `DrowningWearable` folder already contains all nine `.ino` tabs and eight headers; the IDE loads the other tabs automatically. Keep the whole folder together when copying it elsewhere. Select **ESP32S3 Dev Module**, **8 MB flash**, **Default 8MB partition scheme**, **OPI PSRAM**, and **USB CDC On Boot: Enabled** for the ESP32-S3-DevKitC-1-N8R8. Connect the board by USB, select its port, then click **Upload**.

Keep exactly one `setup()` and one `loop()`, both in the primary tab. Arduino combines the primary tab first, followed by the other tabs alphabetically. Every tab includes `FirmwareShared.h` for library includes, shared `extern` declarations and function prototypes; each global has one definition in its owning tab. This also supports editors that put another primary tab first. Do not `#include` the `.ino` files. [Arduino sketch preprocessing](https://docs.arduino.cc/arduino-cli/sketch-build-process/)

For **Wokwi**, use an **ESP32-S3-DevKitC-1** project. Replace the entire `sketch.ino` with the contents of `Software/DrowningWearable/DrowningWearable.ino`, then add the remaining `.ino` tabs and every `.h` file from `Software/DrowningWearable/`. Omit the `DrowningWearable.ino` tab in this layout so there is only one copy of `setup()`/`loop()` and the global definitions. Use [libraries.txt](../Software/DrowningWearable/libraries.txt) to select the tested library versions. Preserve your project's `diagram.json`; it is separate from the firmware sources. [Wokwi libraries](https://docs.wokwi.com/guides/libraries)

Wokwi currently does not simulate ESP32 Bluetooth, so compiling there does not verify a live BLE connection to the webapp. That requires the physical ESP32. [Wokwi simulation features](https://docs.wokwi.com/guides/esp32#simulation-features)

| Sketch tab | Responsibility |
|---|---|
| `DrowningWearable.ino` | Shared state definitions, startup and main scheduling loop |
| `Actuators.ino` | SOS/Cancel handling, battery checks, vibration and pump task |
| `Bluetooth.ino` | BLE service, callbacks and telemetry packets |
| `Depth.ino` | MS5837 initialization, readings and surface zero |
| `History.ino` | PSRAM allocation, acquisition records and BLE history transfer |
| `I2c.ino` | Checked register transfers |
| `Imu.ino` | BMI270 initialization and readings |
| `Ppg.ino` | MAX30102 configuration, FIFO and HR/SpO2 estimates |
| `SerialCommands.ino` | Serial command parsing |

The [build helper](../Software/build-firmware.ps1) compiles `Software/DrowningWearable/` directly. From the repository root, with the dependencies already downloaded to the indicated directory, compile using:

```powershell
./Software/build-firmware.ps1 -LibraryDirectory ./tmp/revf-build/libraries -BlueRoboticsLibraryDirectory ./tmp/revf-build/libraries/BlueRobotics_MS5837_Library-1.1.1
```

`-BlueRoboticsLibraryDirectory` explicitly selects the official library when multiple packages provide `MS5837.h`. If the libraries are already installed in the configured Arduino sketchbook, the directory options can be omitted. The helper compiles only; it does not install libraries or upload firmware.

### Recent history and ML memory budget

The N8R8 configuration has 8 MiB PSRAM. Startup reserves two aligned **1 MiB workspaces** for future ML, then attempts a **4 MiB sensor ring**, preserving at least **1 MiB of free PSRAM** at allocation time. The usual 6 MiB total reservation leaves roughly 2 MiB for other allocations, less SDK overhead. The ring falls back to 2 MiB or 1 MiB if necessary. These allocations explicitly require PSRAM; they never consume a multi-megabyte internal SRAM buffer. Startup prints actual capacity, reserved ML bytes, free PSRAM and largest free block. If PSRAM or the required allocations are unavailable, the app reports live-only operation. [Espressif external RAM](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/api-guides/external-ram.html)

| Recorded source | Scheduling/sample budget | Record contents |
|---|---:|---|
| BMI270 | Up to 50 Hz | Acceleration and angular velocity |
| MAX30102 | 25 Hz FIFO output | Red/IR raw samples and current valid HR/SpO2 estimates |
| MS5837 | Up to 5 Hz | Pressure, depth, temperature and surface baseline |
| System | Up to 5 Hz | Battery, controls, overflow/error counters |

Every record is 32 bytes, including sequence, validity flags and a 64-bit microsecond timestamp. At the 85 records/s scheduling budget, storage is `85 × 32 = 2,720 bytes/s`. A 4 MiB ring holds **131,072 records**, approximately **25.7 minutes**; 2 MiB and 1 MiB hold about 12.9 and 6.4 minutes. Reset/error/calibration events use additional records. The official MS5837 library blocks during conversion, so the IMU's actual polling rate is below its 50 Hz target. PPG timestamps are estimated from FIFO order and flagged accordingly. These are capacity estimates, not guarantees of precise sampling intervals.

Once full, each append overwrites one oldest record in constant time. Recording continues while disconnected and while history transfers. History stays in RAM only and is lost on reset or power loss, as requested. No flash logging is used.

The two workspaces are available through `historyMlWorkspace(0)` and `(1)` (check for null); their size is `Config::ML_WORKSPACE_BYTES`. No model inference or drowning logic is added. Two 1 MiB workspaces are a provisional budget: final model files, tensor/operator requirements, stack usage and measured inference latency still determine whether the selected models fit and maintain sampling performance.

Update the web app and firmware together. On connection, the app saves a fixed snapshot into local IndexedDB before enabling live telemetry. It acknowledges each checksummed block only after the storage transaction completes. Background transfers then copy the samples recorded during the initial download and subsequent live use. Reconnection resumes at the saved checkpoint; overwritten ranges appear as explicit gaps. A slow BLE link may still fall behind the ring. The app lists saved sessions and exports CSV locally. The [history protocol](../Software/HISTORY_PROTOCOL.md) defines the added characteristics; the existing live packet layouts are unchanged.

### Pressure reading validation

Official release 1.1.1 uses global `Wire`, takes no argument in `init()`, and returns `void` from `read()`. The sketch initializes the shared GPIO8/GPIO9 bus first, selects the MS5837-30BA model, and retains surface-pressure zeroing and BLE readings. Its pre/post-read I2C acknowledgements and finite/range checks detect some failures, but cannot detect every failed transaction inside the library or prove that a plausible result is correct. [Release 1.1.1 API](https://raw.githubusercontent.com/bluerobotics/BlueRobotics_MS5837_Library/1.1.1/MS5837.h)

Validate temperature, pressure and depth against known references before relying on them. The unmodified 1.1.1 compensation code computes `dT*dT` in 32-bit arithmetic in its warm-temperature branch, which can overflow before division; successful communication and range checks do not establish compensation accuracy. Surface zeroing does not validate the conversion across temperatures and depths. [Upstream compensation source](https://raw.githubusercontent.com/bluerobotics/BlueRobotics_MS5837_Library/1.1.1/MS5837.cpp)

### Shared bus, power and unused-pin assumptions

- All three sensors share one 400 kHz `Wire` bus, with addresses BMI270 `0x68`, MS5837-30BA `0x76`, and MAX30102 `0x57`. BMI270 ADR and CS retain the Rev F breakout configuration. This assumes a SparkFun BMI270 breakout, GY-MS5837-30BA board, and HiLetgo MAX30102 breakout, rather than bare ICs or different board variants.
- I2C pull-ups must be compatible with 3.3 V logic. Rev F's R7/R8 are DNP because breakout pull-ups are expected; total pull-up loading, rise time and actual pull-up rails remain assembly checks. Firmware does not test those electrical characteristics. The exact HiLetgo board's regulator and level-shifter arrangement still needs verification.
- The ESP32 and sensors use the intended 3.3 V rail. GPIOs are signal pins, not 5 V/12 V power inputs. Do not connect VBAT directly to GPIO1: the assumed divider is required, and its ADC reading must be checked against a meter.
- Grounds are common at TP4056 protected `OUT-`, including ESP32, sensors, regulator outputs and MOSFET sources. TP4056 battery `B-` is a separate protection node; do not assume it is interchangeable with `OUT-`.
- Rev F supplies external 3.3 V through TPS63020 and JP1. Its USB arrangement requires removing JP1 when USB supplies the ESP32, avoiding two sources feeding the same rail. The board's 5 V header remains unused by the application. GPIO16 does not switch the boost converter's enable input.
- Q1 and Q2 are assumed to be the drawn active-high, low-side N-MOSFET drivers, each with a 100-ohm gate resistor and a 100k gate-to-ground pulldown. Firmware initially commands both gates LOW; the external pulldowns are needed during reset, before firmware runs.
- Pump positive goes to regulated +12 V; pump negative goes to Q2 drain; Q2 source goes to common ground. Retain a correctly rated flyback path across the pump: the drawn D3 cathode/stripe faces +12 V and its anode faces the switched pump-negative node. This routing assumption does not establish D3, Q2 or boost capacity for motor transients.
- All other GPIOs are unassigned by the sketch. The design does not repurpose native USB GPIO19/20, the N8R8 octal-PSRAM GPIO35/36/37, or boot-strapping GPIO0/3/45/46. GPIO43/44 are not used for sensors or actuators. Module-internal flash/PSRAM connections are left alone. See the [Espressif board guide](https://docs.espressif.com/projects/esp-idf/en/v4.4.3/esp32s3/hw-reference/esp32s3/user-guide-devkitc-1.html) and [strapping-pin guidance](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/schematic-checklist.html).

### Manual test behavior and limits

SOS starts vibration on GPIO7. After 10 seconds, vibration stops and the pump on GPIO16 runs until Cancel. Cancel wins at every stage, including exactly at the pump-start deadline. Releasing SOS does not cancel. A held SOS at boot or after an abort does not restart the sequence: release it while idle, then press again. Serial `sos` and `off` request the same test/stop actions; `z` zeroes depth at the surface. Sensors never start the pump.

The existing battery interlock remains: below 3.4 V, or with an invalid battery reading, both outputs stop and cannot start. This threshold is inherited from the Rev F design; it does not measure the 12 V rail or certify the pump's supply. USB-only sensor testing without a valid battery-divider reading will not start the actuators. `ENABLE_ACTUATOR_TESTS` defaults to true as requested; it may be set false for sensor-only work.

BLE protocol version 1, its UUIDs and 20-byte packets remain unchanged. System bit 1 now names pump-test enable and bit 2 names the pump command; bit 5 is vibration and bit 6 is countdown. These are commanded electrical states, not measured motion or water flow. The webapp provides no pump command.

The pump listing recommends 12 V within a 9-14 V operating range and gives up to 350 mA running current and 150 mA unloaded current. Startup/stall current and a submersion rating are not established by that listing. It also specifies no pressure switch and prohibits a blocked outlet. Thus the requested run-until-Cancel mode is a manual bench behavior, not backpressure protection for the reacting bladder. [Pump listing](https://www.amazon.com/dp/B09XH1GYYQ)

Loaded 12 V regulation, startup/stall current, flyback protection and Q2's conduction at 3.3 V gate drive still need bench validation. IRLB8721 on-resistance is specified at higher gate voltages, so retaining GPIO16 is not proof of driver suitability. The vibration motor's supply/rating and the pump's enclosure/backpressure arrangement also remain hardware checks. [IRLB8721 datasheet](https://www.infineon.com/dgdl/Infineon-IRLB8721-DataSheet-v01_01-EN.pdf?fileId=5546d462533600a40153566056732591), [U3V40F12 specifications](https://www.pololu.com/product/4016)

## Historical component list (predates Rev F)

The list below is retained for reference. Its ECG, servo, regulator and strobe details do not describe the current pump firmware; use the current assumptions above for assembly planning.

## Controller and Sensors

- **ESP32-S3-DevKitC-1-N8R8 (x1):** Main microcontroller that runs everything.
- **SparkFun BMI270 IMU breakout (x1):** Motion and orientation sensing.
- **MS5837-30BA depth sensor (x1):** Measures depth and water pressure.
- **AD8232 ECG module (x1):** Reads heart signals.
- **Dry ECG electrodes, RA / LA / RL (x1 set):** Skin contacts for the ECG.

## Power

- **LiPo battery, 3.7 V 2000 mAh 2C (x1):** Main power source.
- **TP4056 charger module (x1):** Charges and protects the battery.
- **Pololu S8V9F3 regulator (x1):** Provides steady 3.3 V for the ESP32 and sensors.
- **XL6009 boost module (x1):** Steps battery voltage up to 5 V for the servo.
- **470 µF 16 V electrolytic capacitor (x1):** Absorbs the servo’s startup current so the ESP32 doesn’t reset.

## Actuation and Alerts

- **Servo motor (x1):** Opens the valve that inflates the bladder.
- **3-pin servo header (x1):** Connects the servo (signal to GPIO 16, +5 V, GND).
- **AO3400A N-MOSFET, SOT-23 (x2):** Switches the vibration motor and the strobe.
- **100 Ω resistor (x2):** Limits current into the MOSFET gates.
- **100 kΩ resistor (x2):** Keeps the MOSFETs off during startup.
- **SS14 Schottky diode (x1):** Protects against voltage spikes when the motor shuts off.
- **Coin vibration motor (x1):** Vibration alert for the diver.
- **Evan Designs strobe (x1):** Visual distress light.

## Controls

- **Pushbutton (x2):** Cancel and SOS buttons.
