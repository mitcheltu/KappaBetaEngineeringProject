# Sensor Fusion Design

## Purpose

This module processes timestamped windows of sensor data and produces a fused
risk score `R` for the smart life jacket prototype.

The module is deliberately independent of Arduino and ESP-IDF. Sensor drivers
will later populate the input windows, while this module remains testable on a
desktop build.

## Processing pipeline

```text
raw timestamped windows
        |
        +--> depth validation --> Hampel rejection --> EMA smoothing
        |                                      |
        |                                      +--> rolling depth regression
        |
        +--> IMU validation --> acceleration/angular-motion features
        |                                      |
        |                                      +--> future orientation estimator
        |
        +--> MAX30102 validation --> future pulse/SpO2 extraction
                                               |
                                               +--> future H/O mappings

M, A, H, O, q --> quality-aware risk fusion --> R
```

## Input conventions

- Every sample has a monotonically increasing timestamp in microseconds.
- Depth values passed to this module are metres, positive downward.
- MS5837 pressure-to-depth conversion and Start Dive pressure-baseline
  handling belong in the sensor adapter before depth filtering.
- IMU acceleration is descriptive input only. It must not be integrated into
  velocity in this module.
- MAX30102 input contains raw red and infrared readings. Pulse and SpO2
  extraction is not implemented yet.

## Depth processing

The depth window is processed in two stages:

1. A Hampel filter identifies isolated spikes using a configurable local
   median, MAD, and outlier multiplier.
2. An EMA smooths accepted values using a configurable alpha.

Rejected samples retain their timestamps and are marked as rejected. If an EMA
value already exists, a rejected sample carries forward the previous filtered
value; it does not become a new measurement.

Sensor-provided invalid samples are retained but excluded from filter
neighborhoods. If a previous EMA exists, the output is marked as carried
forward so downstream code can distinguish a held value from a new sensor
measurement. Quality flags are preserved alongside timestamps and values.

The filter radius, Hampel multiplier, and EMA alpha are required configuration
values. No project-specific defaults are selected.

## Ascent estimation

The caller supplies a rolling window of cleaned depth samples. Ordinary
least-squares regression estimates depth slope in metres per second.

- Positive slope: descending
- Negative slope: ascending

This slope is a feature, not yet an ascent-risk score. The mapping to `A` and
any persistence or action thresholds remain future configuration.

## IMU processing

The current implementation calculates descriptive features from acceleration
and angular velocity magnitudes. It also estimates roll and pitch from the
mean acceleration vector over the window, treating that vector as a gravity
reference. This is context only and may be unreliable during strong motion.

Samples marked sensor-invalid are excluded from the feature statistics, and
the feature result reports both total and valid sample counts.

Yaw is explicitly unavailable: a 6DoF IMU has no absolute heading reference,
so the module does not integrate gyro data into a long-term yaw estimate.

The mapping from IMU features to normalized motion-risk score `M` remains
outside the current implementation.

## MAX30102 processing

Raw red and infrared windows are accepted and validated. The current step
calculates mean and standard-deviation features for both channels. These are
descriptive preprocessing outputs, not pulse or SpO2 measurements. It also
reports red and infrared perfusion-index features as AC variation divided by
the channel DC mean.

Samples marked sensor-invalid are excluded from those statistics while the
total and valid sample counts remain available to later quality logic.

Pulse and SpO2 extraction is intentionally left unimplemented until the
project selects the algorithm, sampling assumptions, calibration assumptions,
and validity rules. The code accepts caller-supplied pulse and SpO2 extractors;
missing extractors or missing metric results prevent a valid MAX30102 feature
result. No thresholds or calibration coefficients are embedded.

The mappings from pulse and SpO2 measurements to `H` and `O` are also left
open.

The end-to-end `SensorFusion` path accepts the MAX30102 extractors and runs
them before the risk-mapping callbacks. This keeps raw-window processing,
physiological metric extraction, normalized risk mapping, and final fusion as
separate testable stages.

## Risk-mapping interfaces

The implementation exposes four caller-supplied mapping callbacks:

- IMU features to `M`
- Ascent slope to `A`
- Pulse features to `H`
- SpO2 features to `O`

Each callback must return a normalized value in `[0, 1]` once its project
rules are implemented. An absent callback or absent mapped value prevents a
final score. Invalid or insufficient sensor windows also prevent a final
score, even if a callback returns a value. This keeps threshold and baseline
decisions outside the fusion engine without allowing placeholder mappings to
mask sensor failures.

## Risk fusion

When all normalized components and `q` are present, the module calculates:

```text
R = (0.37M + 0.33A + 0.15qH + 0.15qO) / (0.70 + 0.30q)
```

All five inputs must be within `[0, 1]`.

- `q = 0` is a valid known-unusable MAX30102 signal. In that case, pulse and
  SpO2 contribute zero and motion/ascent are renormalized by the denominator.
- Missing `q` is different from `q = 0`; the module reports that quality is
  unavailable and does not produce a final score.
- The result is a risk score, not a probability of drowning.

No distress threshold, persistence period, actuator action, or inflation
decision is implemented in this module.
