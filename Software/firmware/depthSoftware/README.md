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
40 ms total and does not expose its calibration or conversion primitives. This
module schedules the same MS5837-30BA commands itself, with a 20 ms deadline
for each conversion, so the configured 200 ms sample interval retains the
initial 5 Hz cadence without blocking the loop. Every command, read request,
and returned byte count is checked before a sample can be published.

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

sensor.service(); // Call on every loop pass; it never waits for conversion.
const DepthReading& reading = sensor.reading();
```

`DepthSensor` owns the MS5837 protocol, surface-pressure baseline, filter state,
sample timing, and fixed-size histories. Call `service()` on every loop pass;
it advances one of the idle, pressure-conversion, or temperature-conversion
states without waiting. It does not own user profile data, risk scoring, or
sensor-fusion weights.

Each `DepthReading` preserves:

- timestamp in microseconds;
- raw pressure in Pa and temperature in °C;
- surface baseline pressure;
- raw relative depth and filtered relative depth in metres;
- filtered depth rate in m/s, positive downward and negative upward;
- `sensorValid`, `depthValid`, `rateValid`, and descriptive quality flags.

The processing sequence is:

1. Start a pressure conversion, then a temperature conversion, each with a
   20 ms deadline; neither conversion blocks the loop.
2. Read both ADC results and apply the MS5837-30BA temperature compensation.
3. Convert pressure to relative depth using
   `(pressure - surfacePressure) / (waterDensity * 9.80665)`.
4. Reject isolated depth spikes with a Hampel filter.
5. Smooth accepted depth samples with an EMA.
6. Estimate rate with least-squares regression over timestamped filtered samples
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
                            projectedAreaM2);
```

It calculates the natural sinking speed from the supplied SI-unit profile and
can compare it with a valid `DepthReading` using the absolute rate difference
and normalized `sinkRiskScore`:

`max(0, 1 - abs(measuredRate - naturalSinkingSpeed) / naturalSinkingSpeed)`.

The score is `1` at an exact rate match and `0` when the mismatch reaches the
natural sinking speed. It is available only when the depth-rate reading and
model are valid and the modeled natural speed is positive; it is intended as a
sensor-fusion risk component, not a calibrated drowning probability.
The drag coefficient is permanently fixed at `0.85` in
`SinkRateConfig::DRAG_COEFFICIENT` until hydrodynamic calibration is validated;
it is not caller-configurable. No production mass, height, displaced volume, or
projected-area values are hardcoded in this example.
