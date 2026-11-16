# IMU CNN firmware build

This ESP-IDF project builds the existing `microcontrollerFinal` Arduino
firmware, including its IMU risk evaluator and embedded float32 CNN. Arduino
is used as an ESP-IDF component so the firmware can link Espressif's official
TFLite Micro component.

## Pinned runtime

- ESP-IDF: 5.5.2
- Arduino-ESP32 component: 3.3.7
- `espressif/esp-tflite-micro`: 1.4.1
- The TFLM component brings its Espressif NN dependency through Component
  Manager.
- The local Arduino library component checks BlueRobotics MS5837 1.1.1,
  NimBLE-Arduino 2.5.2 and SparkFun MAX3010x 1.1.2. The firmware uses its own
  checked BMI270 transport, so it does not compile the installed SparkFun
  BMI270 library.

The `.cc`, `.h` and inference config files here are the firmware copy of the
bundle at `software/models/swim_imu_esp32_deployment`. Run `./sync_model.sh`
after exporting a replacement bundle there.

## Build

Install ESP-IDF 5.5.2 and the pinned Arduino libraries. The library component
defaults to `~/Documents/Arduino/libraries`; set `ARDUINO_LIBRARIES_DIR` if the
libraries live somewhere else. From this directory:

```sh
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
idf.py -p <PORT> flash monitor
```

`sdkconfig.defaults` selects the ESP32-S3, 8 MB flash, octal PSRAM, BLE NimBLE,
1 kHz FreeRTOS ticks, and manual Arduino startup. `partitions.csv` matches the
existing 8 MB Arduino partition layout. Component Manager writes a
`dependencies.lock` on the first configuration; retain that file to pin
transitive component versions for later builds.

The evaluator allocates TFLM's tensor arena from the first 1 MiB PSRAM workspace
reserved by `initializeHistory()`. The model registers `CONV_2D`,
`EXPAND_DIMS`, `FULLY_CONNECTED`, `LOGISTIC`, `MAX_POOL_2D`, `MEAN` and
`RESHAPE`; setup validates the float32 `[1, 60, 6]` input and `[1, 1]` output.

## Sensor-unit caveat

The deployment artifacts specify axis order and normalization constants but do
not record the training sensor units. The current BMI270 firmware produces
acceleration in g and angular velocity in degrees/second. The evaluator passes
those values directly to `(raw_value - mean) / scale`, following the exported
contract; it does not silently convert units. Confirm the training pipeline's
units before interpreting model scores on hardware.
