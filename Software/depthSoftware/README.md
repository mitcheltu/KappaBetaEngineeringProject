# Depth sensor reference sketch

This standalone sketch exercises the same `DepthSensor` and
`SinkRateEstimator` interfaces used by `microcontrollerFinal`. The canonical
implementation is in `../microcontrollerFinal/DepthSensor.h/.ino` and
`SinkRateEstimator.h/.ino`; the files in this folder are thin wrappers plus a
small serial-driven example.

## Hardware and timing

- Board: ESP32-S3-DevKitC-1-N8R8
- I²C: GPIO8/GPIO9 at 400 kHz, shared with the BMI270
- Sensor: GY-MS5837-30BA containing an MS5837-30BA at `0x76`
- Logic: 3.3 V; keep the breakout PCB dry while the pressure port contacts water
- Library: BlueRobotics MS5837 Library 1.1.1

The pinned library's `read()` path performs blocking D1/D2 conversions of about
40 ms total. The configured 200 ms sample interval therefore targets about
5 Hz; this implementation does not claim 20 Hz. The library also does not
return individual conversion-transfer status, so the module combines I²C ACK
checks before and after a read with pressure and temperature range checks.

## Running the example

Open `depthSoftware.ino`, select the ESP32-S3 board, and open Serial Monitor at
115200 baud. The example uses these commands:

| Command | Effect |
|---|---|
| `start dive` | Capture a fresh surface-pressure baseline and reset dive filters/history |
| `stop dive` | Invalidate the baseline, filtered depth, and depth rate |

Before `start dive`, pressure and temperature can be reported, but relative
depth remains invalid because no surface baseline exists.

## `DepthSensor` interface

`DepthSensor.h` defines the data model and public interface. `DepthSensor.ino`
contains the implementation.

```cpp
DepthSensor sensor(DepthSensorConfig{});

sensor.begin(Wire, 0x76);
sensor.startDive();

if (sensor.due(nowUs)) sensor.update();
const DepthReading& reading = sensor.reading();
```

`DepthSensor` owns the MS5837 driver, surface-pressure baseline, filter state,
sample timing, and fixed-size histories. It does not own user profile data,
risk scoring, or sensor-fusion weights.

Each `DepthReading` preserves:

- timestamp in microseconds;
- raw pressure in Pa and temperature in °C;
- surface baseline pressure;
- raw relative depth and filtered relative depth in metres;
- filtered depth rate in m/s, positive downward and negative upward;
- `sensorValid`, `depthValid`, `rateValid`, and descriptive quality flags.

The processing sequence is:

1. Read the MS5837 and use its temperature-compensated pressure result.
2. Convert pressure to relative depth using
   `(pressure - surfacePressure) / (waterDensity * 9.80665)`.
3. Reject isolated depth spikes with a Hampel filter.
4. Smooth accepted depth samples with an EMA.
5. Estimate rate with least-squares regression over timestamped filtered samples
   in the configured 2–3 second window.

Rejected samples retain their raw pressure/depth and receive
`OUTLIER_REJECTED`; they are excluded from the EMA and regression history.
Downstream code should treat readings with `depthValid == false`,
`rateValid == false`, or relevant quality flags as unknown.

## Configuration status

`DepthSensorConfig` keeps the sampling interval, water density, Hampel settings,
EMA alpha, regression window, stale timeout, and validity ranges together.
The current values are implementation defaults and are explicitly
**PROVISIONAL/TBD**; they are not safety-calibrated. Water density is currently
1000 kg/m³ for provisional freshwater/pool use.

## Sink-rate estimation

`SinkRateEstimator` is configured once from an external user/profile owner:

```cpp
SinkRateEstimator estimator(totalMassKg,
                            displacedVolumeM3,
                            waterDensityKgM3,
                            dragCoefficient,
                            projectedAreaM2);
```

It calculates the natural sinking speed from the supplied SI-unit profile and
can compare it with a valid `DepthReading` using the absolute rate difference.
No production mass, height, displaced volume, Cd, or projected-area values are
hardcoded in this example.
