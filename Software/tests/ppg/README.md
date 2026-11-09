# Standalone PPG verification

Run from the repository root with a C++17 compiler:

```powershell
./Software/ppgSoftware/test.ps1
```

The script compiles with `-O2 -Wall -Wextra -Werror -pedantic`, keeps assertions
enabled, and runs these suites. Never add `-DNDEBUG`.

| Test | Checks |
|---|---|
| `oxygen_concern_test.cpp` | 651 Python golden rows / 19 cases, identical alert states, numerical concern, acknowledge/reset/configuration |
| `ppg_window_quality_test.cpp` | Ring wraps/order, timestamp gaps, invalid data, qualification evidence/identity/expiry/revocation, valid flat windows |
| `ppg_pulse_test.cpp` | Python-reference features, all model scores and decisions, peak indices/prominence, invalid inputs |
| `ppg_hardware_test.cpp` | Real `Ppg.ino` against fake I2C/queue boundaries: configuration, decoding, timing, overflow, short reads, no-data timeout |
| `ppg_analysis_test.cpp` | Real `Analysis.ino` with virtual time/queues: 4:1 Maxim inputs/cadence, one-use oxygen estimates, stale readiness, wearer/gap resets, latch/acknowledgment |

The hardware harnesses replace only boundary APIs; they include the real
Arduino acquisition and analysis tabs. Analysis is exercised with a virtual
stream, including real model inference and a simulated late result. These tests
do not replace a target firmware compile or physical-board timing validation.
Actual task scheduling, the physical LED, and sensor analog performance still
require device testing.

## Regenerate oxygen reference fixtures

Requires Python and NumPy. The checked-in reference is checked against the
current notebook's AST and constants before fixtures are generated:

```powershell
python Software/tests/ppg/generate_oxygen_golden.py
```

`oxygen_golden.csv` records inputs, validity, seconds, per-second concern, and
active alerts. Cases include missing readings, gaps, duplicates/backward time,
uint32 timestamp rollover, anchors, confirmation, recovery and latching.

## Export models and regenerate pulse fixtures

Use **trusted local** outputs from `04_pulse_loss_detector.ipynb`; loading
joblib/pickle files executes Python code. This step does not retrain models or
choose new thresholds. Python needs NumPy, SciPy, pandas, scikit-learn, joblib,
and PyTorch matching the source artifact environment recorded in the manifest.

```powershell
python Software/ppgSoftware/tools/export_pulse_models.py tmp/kaggle-qa/v3/out04_pulse
python Software/ppgSoftware/tools/generate_pulse_golden.py tmp/kaggle-qa/v3/out04_pulse
./Software/ppgSoftware/test.ps1
```

The source artifact directory contains `pulse_loss_configuration.json`,
`pulse_loss_feature_models.joblib`, and `pulse_loss_cnn_1d.pt`. The generated
header and source hashes are checked in, so ordinary C++ tests do not need these
git-ignored local binaries or Python. Future regeneration does require the
original trusted exports or a new explicitly identified notebook run.

`pulse_waveforms.csv` supplies complete float32 windows; `pulse_expected.csv`
supplies ten features and model scores. The peak CSVs check indices and
prominences separately, including plateaus and distance/prominence ordering.
`pulse_golden_manifest.json` records versions, source hashes, fixture hashes,
and tolerances. These deterministic synthetic cases check software arithmetic;
they are not clinical labels or evidence of detector effectiveness.

The pulse suite checks ten full waveforms across all four models and both
operating points, six ordinary peak cases, and a separate intentionally
unsupported equal-height tie-order case. The latter must be flagged for
abstention and is not counted as a parity pass. Model decisions match exactly
for accepted fixtures; feature absolute tolerance is `1e-7`, logistic/tree
score tolerance `1e-8`, and CNN score tolerance `1e-5`.

The filtering reference is SciPy `sosfiltfilt` with default odd padding and
initial states. Peak distance filtering precedes prominence filtering. See the
primary [SciPy filtering documentation](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.sosfiltfilt.html)
and [peak documentation](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.find_peaks.html).

For individual tests outside PowerShell, use the same C++17 flags, include
`Software/ppgSoftware`, and run from the repository root. The hardware test also
needs `-I Software/tests/ppg_hardware_stubs`. MSYS2 users should put
`C:/msys64/ucrt64/bin` on PATH before executing their binaries.
