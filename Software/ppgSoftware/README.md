# MAX30102 PPG reference sketch

Open `ppgSoftware.ino` in Arduino IDE. This standalone ESP32-S3 sketch contains
the MAX30102 acquisition path, HR/SpO2 estimation, oxygen concern rule, and PPG
pulse-loss research models. It does not change `microcontrollerFinal` or the
demo sketches. All `.ino` tabs in this directory compile together.

The oxygen and pulse models are research outputs, with source configurations
marked **`deployable: false`**. A matching software port is not validation of
chest PPG in water or a drowning/blackout detector.

## Hardware and setup

| Item | Setting |
|---|---|
| Board | ESP32-S3-DevKitC-1-N8R8 / ESP32S3 Dev Module |
| I2C | SDA GPIO8, SCL GPIO9, 400 kHz, 3.3 V logic |
| Sensor | MAX30102, address `0x57`, red + IR |
| Acquisition | 100 Hz raw, **hardware averaging off**, 18-bit ADC |
| Local indication | Onboard RGB LED GPIO48; change to GPIO38 for DevKitC-1 v1.1 |
| Acknowledge | Normally open button from GPIO5 to GND |
| Serial | 115200 baud, newline |
| Library | SparkFun MAX3010x Pulse and Proximity Sensor Library **1.1.2** |

Use 8 MB flash, Default 8MB partitions, OPI PSRAM, and USB CDC On Boot enabled.
The project documents ESP32 core 3.3.7; see the verification section for the
version actually compiled on this machine. `libraries.txt` records the sensor
library version. The standalone sketch has no NimBLE or esp-dsp dependency.

The existing final sketch enables hardware averaging and emits 25 Hz FIFO
samples. This sketch explicitly selects `setup(..., 1, 2, 100, 411, 4096)` and
checks the mode, averaging, rate, and LED registers after initialization.

## Data flow and ownership

```text
MAX30102 FIFO, 100 Hz
  sensor task: checked I2C reads -> acceptPpgSample() -> bounded raw queue
    analysis task, single owner of all processing state
      +-> PpgDecimator<4> -> Maxim 100-sample window -> HR / SpO2 every 1 s
      |                                                 -> OxygenConcern
      +-> PpgWindow: 1600 raw IR samples -> copy every 200 new samples
                                              -> QualityGate
                                              -> PulseLoss
      +-> latched alarm state -> local RGB LED
      +-> status mailbox -> Serial from the Arduino loop
```

The sensor task runs on core 1 at priority 4; analysis runs on core 1 at priority
2. Only the sensor task owns `Wire`. It never runs Maxim, FFTs, models, or Serial
printing. A static queue holds 512 samples; a full queue drops the sample and
changes a generation counter. Analysis discards old generations, resets its
windows, and revokes wearer qualification. It never joins data across a gap.

The hardware FIFO holds 32 samples, about 320 ms at 100 Hz. Polling is every
5 ms, with a conservative 250 ms service deadline, checked overflow counter,
200 ms no-data timeout, and checked reads. Initialization retries every 5 s.
See the [MAX30102 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX30102.pdf).

Sample timestamps reconstruct the nominal sensor clock in 10 ms steps. Separate
host arrival timestamps determine freshness, so oscillator drift and FIFO poll
jitter do not invalidate a continuous stream. Gaps, invalid contact readings,
and clipping reset both optical windows. A finite flat signal above the contact
threshold remains eligible: a missing pulse must not disable its own detector.

The MAX30102 contact/ADC thresholds are provisional engineering gates, not proof
of correct placement or accurate SpO2. HR/SpO2 need four seconds of accepted
data; pulse processing needs a full sixteen seconds. Failed transfers and stale
estimates produce unavailable results, not zero-risk results.

## OxygenConcern.h

This allocation-free header ports notebook 03's **absolute `concern_level`**
variant. `references/oxygen_reference.py` freezes the notebook reference, and
the fixture generator verifies its functions/constants against the notebook.
Person-relative baseline and trend variants are not selected by this sketch.

Defaults: 80 to 60 percent anchors, a three-sample median, exponential filtering
with a three-second time constant, enter at 0.5, exit at 0.25, three seconds of
confirmation, and five seconds of recovery. `autoClear=false` keeps device
alerts latched. Recovery is used only if explicitly enabled for replay.

```cpp
Ppg::OxygenConcern oxygen(Config::oxygenConfig());
oxygen.update(spo2, valid, seconds); // once per second; true means concern ready
float concern = oxygen.concern();   // NaN when unavailable
bool alarm = oxygen.active();      // latch survives invalid readings
oxygen.acknowledge();
```

Invalid flags, nonfinite values, and SpO2 outside `(0,100]` are missing input.
Missing/duplicate/skipped timestamps restart the median and confirmation timers,
while preserving the exponential state and an active latch, matching the Python
reference. Acknowledgment clears the latch and timers, preserving filter state;
a persistent condition can confirm again. Configuration is in `FirmwareConfig.h`
through `oxygenConfig()`; algorithm defaults live in `OxygenConfig`.
Each estimate is consumed at most once; a cached reading cannot advance the
median or confirmation timer again just because it has not expired yet.

## PulseLoss.h and exported models

The input is exactly 1,600 finite IR samples at 100 Hz, in acquisition polarity.
The implementation follows `04_pulse_loss_detector.ipynb`:

- Third-order 0.5–8 Hz Butterworth SOS filter, forward/backward over the complete
  buffered window, including SciPy's odd extension and initial conditions.
- Percentile normalization and peak detection with distance and prominence.
- The same ten features and order, including 512-point Welch spectral purity.
- Rule, scaled logistic regression, boosted-tree inference, and handwritten CNN
  inference using the actual exported model parameters.

`PulseLossParams.h` is generated from the local notebook run's joblib, PyTorch,
and configuration exports; the export manifest records provenance and hashes.
There are no substitute or randomly initialized production weights. The default
is logistic regression with the exported `HighSpecificity` threshold. Select a
different `PulseModel` or operating point in `FirmwareConfig.h`.

The notebook's WildPPG adapter negates that dataset's IR waveform. This sketch
preserves MAX30102 ADC polarity by default; verify it against the intended
model input at the actual placement, and set `PULSE_INVERT_IR` if needed. The
preprocessing/model is not assumed to be invariant to inversion. This setting
affects only the pulse ring, not the red/IR values passed to Maxim.

```cpp
static Ppg::PulseLoss detector; // fixed workspace; keep off a small task stack
auto result = detector.evaluate(window, Ppg::PulseModel::Logistic,
                                Ppg::PulseOperatingPoint::HighSpecificity);
// result.valid, result.features[10], result.score, result.threshold, result.alert
```

A portable radix-2 FFT keeps the host and device code identical and removes an
extra Arduino library dependency. This implements the requested Welch path
without calling `dsps_fft2r`; switching to esp-dsp requires rerunning the golden
tests. Zero-phase filtering is possible because this is buffered analysis, but
it still has a sixteen-second observation window and a two-second update step.

SciPy delegates equal-height peak priorities to NumPy's unstable sort, so some
competing ties have no portable ordering across builds. `PulseLoss` reports
`ambiguousPeakTies=true`, `valid=false`, and a NaN score for those windows before
running a model. This deliberate abstention avoids silently changing trained
features. The test suite records one known ordering divergence separately from
the passing parity cases. Unequal heights are not treated as ties by tolerance.

## Quality gate: an explicit current limitation

`QualityGate.h` is a **reference-verified wearer qualification gate**, not the
learned model in notebook 02. That model predicts agreement of optical HR with
ECG; it requires red/IR features plus motion/jerk/correlation features from an
accelerometer. It cannot establish pulse visibility using only this IR ring.
The offline ECG/PPG beat ratio in notebook 04 is also not a live PPG-only model.

Pulse inference is disabled until an operator independently verifies that the
pulse is visible at this wearer's current sensor placement and explicitly
qualifies the session. Qualification has an explicit wearer ID and expiry and
is revoked after contact loss, sample gaps, resets, or the `revoke` command.
It does not depend on detecting beats in the window being classified. This
avoids suppressing a true missing-pulse event with a current-window beat gate.

This is a transparent integration boundary for a future validated visibility
gate. It is not an automatic quality-model implementation or evidence that an
operator qualification remains valid underwater. The oxygen path has its own
measurement-validity checks and does not use this pulse qualification.

## Commands and local indication

| Command | Effect |
|---|---|
| `status` | Print measurements, validity, latches, qualification, drops, gaps, timing |
| `ack` | Acknowledge both local alert latches, as does a new GPIO5 press |
| `revoke` | Clear sample windows and qualification; retains existing alerts |
| `qualify 1 60` | Assert reference verification for wearer 1 for 60 seconds |
| `help` | Show commands |

Qualification requires a full continuous window and a ready sensor. IDs must
be nonzero, and durations are 1–3600 seconds; the maximum is a configurable
software bound, not a physiological validity period. Inspect `qualified=1`
after requesting it. Changing wearers or sensor placement requires revocation
and fresh verification. A different wearer ID also clears the windows; collect
another sixteen seconds and submit qualification again. No profile or
qualification persists across reset. `qualified=1` additionally requires a
fresh window and an available selected model, not just an unexpired session.

LED colors: **red** for either latched alert; **amber** while data or pulse
qualification is unavailable; **blue** when acquisition and qualification are
ready and the latest pulse evaluation produced a valid score. Blue is a
processing-status indication, not a declaration that the wearer is safe.
Acknowledgment does not suppress a continuing condition indefinitely.
No phone or BLE connection is needed for indication. This standalone sketch
does not start BLE; when integrating later, keep NimBLE work on core 0.

GPIO16 (pump) and GPIO7 (vibration) are set LOW during setup and never raised.
There is no automatic or manual pump-start command in this sensor sketch.
The production sketch's physical SOS/Cancel behavior is untouched.

## Scheduling, memory, and verification

Model parameters are const flash data; raw queue, ring, DSP workspace, and CNN
scratch are fixed-size. FreeRTOS allocates task stacks during setup, not during
sample processing. No hot-path heap allocation occurs. This standalone module
does not allocate the main firmware's reserved 1 MiB ML workspace a second time.
The host `PulseLoss` workspace is 65,800 bytes; the ring and snapshot add 12,800
bytes, plus the bounded raw queue, estimator windows, and task stacks.

Status reports pulse analysis elapsed time and the largest observed duration,
including sensor preemption, as `analysis_us` and `max_us`. `overruns` counts
calculations exceeding the configured two-second budget. Results more than
500 ms past the last sample's arrival are withheld. These counters must be
measured on a real board under load; host test timing is not ESP32 WCET evidence.

See [the test instructions](../tests/ppg/README.md) for Python golden generation
and C++17 assertion tests. Do not compile assertion tests with `-DNDEBUG`.
The tests exercise numerical parity, ring ordering, validity gaps, qualification,
and latch behavior. Synthetic verification fixtures are arithmetic test cases,
not training data or physiological ground truth.

For compilation, run `./Software/ppgSoftware/build.ps1` from the repository root,
or use Arduino IDE with the settings above. The script only compiles; it never
uploads firmware or installs libraries. Hardware timing, optical
placement, underwater operation, and local LED visibility still require bench
and device testing.

Verified on 2026-11-09: all five C++17 host suites pass. The final sources also
compile and link with the installed ESP32-S3 toolchain, ESP32 core **3.3.12**, and
MAX3010x **1.1.2**: **523,240 bytes flash** and **121,464 bytes static RAM**.
The latter excludes task stacks allocated during startup. This was a direct
target compiler/linker check; Arduino CLI did not finish its unusually slow
discovery/preprocessing run in this environment. Core 3.3.7 and physical hardware
were not tested, and no firmware was uploaded. Local build commands, source
hashes, and logs are under `tmp/ppg-build/target/`.
