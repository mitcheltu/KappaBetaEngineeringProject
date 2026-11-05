# DrowningWearable Arduino sketch

Open **DrowningWearable.ino** in Arduino IDE. This folder already contains all
nine sketch tabs and eight required headers. Keep the folder named
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
5. Open Serial Monitor at **115200 baud** for startup and sensor status.

`libraries.txt` lists the same pinned libraries for Wokwi; Arduino IDE libraries
are installed through Library Manager. All `.ino` tabs compile together, so
upload the primary sketch once rather than uploading individual tabs.

See the [wiring and firmware guide](../../Electrical/README.md) for pin
assignments, power setup, and manual pump-test behavior. The
[history protocol](../HISTORY_PROTOCOL.md) describes the BLE history service.
The [build helper](../build-firmware.ps1) compiles this same folder from the
command line.
