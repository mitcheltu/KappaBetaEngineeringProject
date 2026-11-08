# IMU risk evaluator

This Arduino CLI sketch owns an `ImuSensor imu` object for the BMI270 and feeds
its readings into the post-processing boundary for the IMU model. The object
exposes named accessors such as `imu.accelerationX()` and `imu.gyroZ()`, plus a
complete `imu.reading()` snapshot and magnitude methods.

The sensor object returns acceleration in `g` and angular velocity in degrees
per second. Call `imu.update()` once per sample period before requesting the
values. The current sketch polls at 100 Hz.

The object also provides `imu.calibrateGyro()` for a stationary gyro-bias
calibration. It does not claim to perform full multi-position accelerometer
calibration; known accelerometer bias values can be supplied with
`imu.setAccelerationBias(x, y, z)`.

The evaluator accepts a fixed-size `60 x 6` tensor with channel order
`[ax, ay, az, gx, gy, gz]`, runs a replaceable 1D-CNN inference method, and
returns `1` when the resulting unsafe score is greater than or equal to the
temporary `0.5` threshold.

The CNN architecture, weights, and inference runtime are intentionally not
defined yet. Replace `Evaluator::runCnn()` when the trained model is available.

The evaluator can be called from firmware code like this:

```cpp
ImuSensor imu;
imu.begin(Wire, 0x68);
if (imu.update()) {
  const float ax = imu.accelerationX();
  const float gz = imu.gyroZ();
}

// Hold the board still while this blocking calibration runs.
imu.calibrateGyro();
```

The serial commands in the sketch are `calibrate gyro` and
`clear calibration`.

Compile this sketch with Arduino CLI using the same ESP32-S3 target currently
used by the repository's firmware build helper:

```text
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=opi,CDCOnBoot=cdc Software/imuSoftware
```
