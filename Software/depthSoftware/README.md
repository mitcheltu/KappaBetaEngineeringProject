# Depth sensor reference sketch

This standalone sketch uses the same `DepthSensor` and `SinkRateEstimator`
interfaces as `microcontrollerFinal`.

- Board: ESP32-S3-DevKitC-1-N8R8
- I²C: GPIO8/GPIO9 at 400 kHz
- Sensor: GY-MS5837-30BA at `0x76`
- Library: BlueRobotics MS5837 Library 1.1.1
- Sampling: approximately 5 Hz with the pinned blocking library; 20 Hz is not claimed

Use Serial Monitor at 115200 baud:

| Command | Effect |
|---|---|
| `start dive` | Capture a fresh surface-pressure baseline and reset filtering |
| `stop dive` | Invalidate baseline, filtered depth, and rate |

The filter settings are provisional/TBD and are not safety-calibrated. No
production user profile is hardcoded for `SinkRateEstimator`; its constructor
must receive mass, displaced volume, density, drag coefficient, and projected
area from an external profile owner.
