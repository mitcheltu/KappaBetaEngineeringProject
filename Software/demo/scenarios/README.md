# Demo scenarios (scripted playback for on-ESP32 inference)

These CSVs are **scripted demo input**. The wearable replays one through its own models
(IMU CNN, pulse `cnn_v5_h`, oxygen `concern_level`) to show the pipeline end to end.
They are not training, test or evidence data. Segments were picked by hand so each
scenario demonstrates one behaviour. Their outcomes say nothing about real-world accuracy.

## The four scenarios (110 s each, after a 30 s arm buffer)

| Id | Name | Motion (IMU) | Pulse waveform (IR) | SpO₂ | Expected on the device |
|---|---|---|---|---|---|
| 1 | Normal swim | SwimWatch crawl | WildPPG `ssx`, healthy | UW 100001, 98 % | Gate opens on 71/108 windows; no vitals alert → **no actuation** |
| 2 | Passive: pulse stops | SwimWatch PassiveDrowning | CinC 2015 `a161l` (expert-confirmed asystole) | UW 100003, 97–98 % | Pulse alert at 88 s with the gate open → **vibration → pump at 88 s** |
| 3 | Active: hypoxia | SwimWatch ActiveDrowning | WildPPG `u7y`, healthy | UW 100006, 73 → 66 % | Oxygen alert at 75 s, gate open → **vibration → pump at 75 s** |
| 4 | Still + hypoxia (gate) | SwimWatch PassiveDrowning (still) | WildPPG `u7y`, healthy | UW 100005, 74 → 66 % | Oxygen alert at 67 s, gate closed → **no actuation** (shows the AND rule, and that it would miss this case) |

Times are seconds after playback starts. They come from `Software/tests/scenario_data_test.cpp`,
which plays every scenario through the firmware's own `Inference`, `Controller` and `DemoSequence` code.

Each channel is one contiguous segment of one real recording. Nothing is synthesized, spliced
or time-warped. The UW desaturations are slow (controlled hypoxemia), so the hypoxia scenarios
start near 74 % instead of compressing a fall that no dataset recorded.

## Sources and licences

| Channel | Dataset | Licence / note |
|---|---|---|
| IMU | SwimWatch smartwatch set, [farhantandia/Smartwatch-SwimStyle-and-Drowning-detection-model](https://github.com/farhantandia/Smartwatch-SwimStyle-and-Drowning-detection-model). It is one of the IMU CNN's three training sets; the others are [swimming-recognition-lap-counting](https://github.com/brunnergino/swimming-recognition-lap-counting) and [AKF/ZPKILP](https://akf.rodbuk.pl/dataset.xhtml?persistentId=doi:10.58145/AKF/ZPKILP). Only SwimWatch has drowning labels. | **No reuse licence in the repository.** These rows are now inside the CSVs and `ScenarioData.h`. Ask the author or keep the repository private before publishing. |
| IR | WildPPG sternum IR (ETH SIPLAB) | Data CC licence per the original README |
| IR | PhysioNet/CinC Challenge 2015 PLETH | ODC-BY |
| SpO₂ | UW/UCSD controlled hypoxemia, commit `c483ae8` | MIT |

Units:

- **IMU:** the SwimWatch file units, m/s² and rad/s, which are the IMU CNN's training units.
  The firmware's live BMI270 reports g and °/s; `Imu::Cnn::toModelUnits` converts if live inference is added later.
- **IR:** the PLETH convention (rises with blood volume). Raw WildPPG intensity is negated.
- **Resampling:** IR is resampled to 100 Hz with notebook 04's `resample_poly`.

`scenarios.json` records every file, record, start time and source hash.

## CSV format (`scenario_<id>_<slug>.csv`)

One row per 10 ms. Columns: `t_ms, ir, ax, ay, az, gx, gy, gz, spo2, spo2_valid`.

- `ir` is on every row.
- The IMU columns appear on every 5th row (20 Hz).
- `spo2` and `spo2_valid` appear on every 100th row (1 Hz). Leave `spo2` blank with `spo2_valid=0` for "estimator unavailable".

## Rebuild

```powershell
# 1) From the datasets (needs the local caches listed in build_scenarios.py):
python Software/testsAndDemo/scenarios/build_scenarios.py
# 2) CSVs -> firmware header (needs only the CSVs; use this after editing a CSV by hand):
python Software/testsAndDemo/scenarios/compile_scenarios.py
# 3) Re-check the scripted outcomes:
g++ -std=c++17 -O2 -Wall -Wextra -Werror -I Software/testsAndDemo/DrowningWearableDemo Software/tests/scenario_data_test.cpp -o tmp/demo-scenario/scenario_data_test.exe
./tmp/demo-scenario/scenario_data_test.exe
# 4) Regenerate the web app fixture after any protocol/catalog change:
g++ -std=c++17 -O2 -I Software/testsAndDemo/DrowningWearableDemo Software/tests/scenario_fixture.cpp -o tmp/demo-scenario/scenario_fixture.exe
./tmp/demo-scenario/scenario_fixture.exe > Software/tests/scenario-protocol-fixture.json
```

`export_imu_cnn.py` regenerates `ImuCnnParams.h` and the IMU parity fixture from
`Software/models/swim_imu_esp32_deployment/imu_cnn_bce.tflite`. It needs `tflite`, plus `ai-edge-litert` for the fixture.

## What the IMU model actually does on SwimWatch

The teammate's gate threshold is 0.998. Across all 3,387 SwimWatch windows in m/s² and rad/s,
the gate opens for:

- **58.5 %** of normal crawl windows;
- 89.8 % of ActiveDrowning windows;
- 40.3 % of PassiveDrowning windows.

On this data it behaves like a "moving vigorously / in the water" signal, not a drowning
detector. Some or all of these recordings were in its training set.
