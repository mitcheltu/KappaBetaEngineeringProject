# Physiological research notebooks

The project compares a PPG pulse-loss/arrhythmia track and a separate oxygen
concern track. These are research experiments, not validated drowning or
blackout detectors. Every model/configuration remains `deployable: false`.

| Notebook | Input and task | Current status |
|---|---|---|
| [02_hr_reliability.ipynb](02_hr_reliability.ipynb) | Replay Maxim optical HR on WildPPG and estimate agreement with ECG | Algorithm unchanged; ECG reference audited across all 16 people |
| [03_oxygen_concern.ipynb](03_oxygen_concern.ipynb) | Compare oxygen rules and a fitted causal level/slope Kalman filter | v5 completed locally, 2026-11-16 |
| [04_pulse_loss_detector.ipynb](04_pulse_loss_detector.ipynb) | Compare waveform detectors on expert-labelled ICU alarms and healthy sternum PPG | v5 full run completed locally, 2026-11-16 |

The notebooks are independent and run on CPU. Notebook 04 uses the **PPG
waveform directly**, not notebook 02's Maxim HR estimate. The fusion library,
BLE contract, and pump control are separate from this research.

## Label and validation policy

Clinical labels come from the original investigators: expert true/false ICU
alarm annotations, protocol groups, and reported outcomes. Automatic ECG/PPG
agreement is an **instrument-comparison heuristic**, not a clinical outcome.
Synthetic crossings and same-stream thresholds are engineering timing
references; they never become invented blackout labels. WildPPG alarms are
counted as false under its healthy-volunteer protocol; that dataset does not
provide adjudicated cardiac-event labels.

All 16 WildPPG participants already appear in saved notebook 02 results.
Notebook 04's v5 split separates people during its own fitting and comparison,
but none is demonstrably untouched across project development. `p5d` was also
used to investigate the ECG counter. Selecting a model on the comparison
participants makes those results selection data. The local run-plan hash
documents the procedure; it is not independent validation or public
preregistration. New effectiveness claims need new participants.

## ECG correction

The v4 counter in notebook 04 overcounted ECG beats. Its claims that chest PPG
shows half the beats, that only 13% of windows show a pulse, and that signal
visibility is the principal bottleneck are withdrawn.

The exact original eight 10-minute clips were replayed with the frozen v4
models and thresholds. Only the ECG reference changed:

| Measurement on 2,344 windows | v4 counter | Corrected counter |
|---|---:|---:|
| Median PPG/ECG beat-count ratio | 0.529 | 0.783 |
| Counts agree within 0.8–1.2 | 13.2% | 37.4% |
| Rule flags among agreeing windows | 0.0% | 2.7% |
| Boosted-tree flags among agreeing windows, high-specificity point | 16.8% | 21.9% |
| Original CNN flags among agreeing windows, high-specificity point | 0.3% | 1.1% |

Overall alarm decisions and rates reproduce v4: the models do not use ECG.
The visibility-conditioned breakdown changes. Matching beat **counts** does
not establish matched beat locations or physiological pulse visibility.

The replacement uses a Pan–Tompkins-inspired band-pass/derivative/energy
envelope, not the complete standard algorithm. A separate audit sampled
beginning/middle/end blocks from all 16 people (4,560 windows). Median
disagreement with an ECG periodicity heuristic fell from 7.78 to 2.60 bpm.
On the 998 more strongly periodic windows, it fell from 2.13 to 1.22 bpm;
96.1% of corrected counts were within 10 bpm. These are comparisons of
automatic estimates, not accuracy against annotated beats.

Notebook 02's dual-detector check qualified 2,325 of those 4,560 sampled
four-second windows (51.0%). Median disagreement with periodicity was 0.42
bpm; 96.0% were within 10 bpm. It does not show the same systematic overcount.
Its reference filtering discards many windows and cannot validate slow,
irregular, or contaminated rhythms. Its historical 12% optical-HR agreement
and 66% wrong accepted readings remain inspected-run results; those models
were not retrained in this audit.

## 04: v5 development comparison

Training uses [PhysioNet/CinC Challenge 2015](https://physionet.org/content/challenge-2015/1.0.0/):
514 PPG-containing records, 123 expert-confirmed alarms. Asystole, bradycardia,
VT, and VF are included; tachycardia is excluded. Each input is the 16 seconds
ending at the alarm, resampled 250 → 100 Hz. The zip MD5 remains
`99a51fefcd72b584a8122ebd7f78dacf`. All 2,056 cached primary/prior windows were
verified exactly against that original archive.

The four v4 baselines retain their hyperparameters and seeds: published
definitions, logistic regression, boosted trees, and the 3,601-parameter CNN.
V5 adds:

- 400 healthy negative windows from eight training participants;
- a PPG-only gate trained against automatic ECG/PPG beat-count agreement;
- leave-one-person-out gate threshold selection at 90% empirical precision,
  with tied scores handled as a complete group;
- a three-member CNN ensemble (7,841 parameters per member) with about a five-second convolutional receptive
  field, temporal bins, early stopping, signal-corruption augmentation, and
  three fixed seeds, with and without healthy negatives;
- a two-of-three-window persistence comparison and a gate based on the
  preceding non-overlapping 16-second window;
- measured ICU sensitivity **after** the gate as well as ungated sensitivity.

Five outer and five inner folds remain record-level because patient IDs are
unavailable. Only 17 true asystole and six true VF records have PPG. The
healthy comparison uses the other eight complete WildPPG recordings. Models
and thresholds are frozen before that comparison is scored.

The inherited selection rule is retained: among the rule and learned models
at their high-specificity operating point, require ungated ICU sensitivity
at least equal to the rule, then minimize gated false-alert starts per
monitored hour, breaking ties by AUROC. This is a **provisional research
ranking**: it mixes ungated sensitivity with gated false alarms and uses an
inspected selection set. Gated sensitivity, monitored fraction, and time
flagged must accompany the ranking. Low alert-start counts alone can hide a
detector that stays continuously on.
These starts are transitions between overlapping two-second window decisions;
they do not simulate the device's latched alert/acknowledgement behavior.

The full run completed 216 learned fits and scored **108.836 hours** from the
eight comparison participants. The inherited rule selects **`cnn_1d` at
`high_specificity`**. Its ungated ICU sensitivity is 40.7% (50/123), falling
to **20.3% (25/123)** with the gate. It produces **15.85 false-alert starts per
monitored hour**, with the gate open for only **21.7%** of recording time
(23.601 hours). The gate's empirical precision on the comparison participants
is 86.7%, below its 90% training target.

The new CNN with healthy negatives raises ungated ICU sensitivity to 56.1%,
but specificity falls to 85.2%; with the gate, sensitivity is 35.8% and the
false-alert rate is 20.51 per monitored hour. It does not win the inherited
ranking. The prior-window gate is closed before 14/17 true asystole alarms and
5/6 true VF alarms. These results do not justify a firmware deployment.

All seven v4 baseline operating-point rows reproduce exactly. The full model
table, gate/persistence tradeoffs, confidence intervals, and source hashes are
in [the completed run report](results/2026-11-16/README.md).

## 03: fitted Kalman filter and oxygen results

The filter estimates level and slope causally at 1 Hz. Its process and
measurement variances minimize Gaussian one-step innovation likelihood on
20 normal-breathing participants; the other 20 controls are excluded from
fitting. It does not fit clinical outcomes. The fitted values are
`Q = 0.00170963` and `R = 0.0833333`. **R reaches its imposed lower bound**,
so this does not measure the MAX30102's error.

The raw observed breath-hold minimum is **83%, not 84%**. Subtracting an
assumed three-percentage-point margin gives an 80% start anchor. That margin
is not a measured device-error bound. The full anchor of 60% remains an
inherited engineering assumption; these healthy outcomes cannot establish it.

Projection only compensates specified processing age. The nominal centre
age of 400 raw samples at 100 Hz, including four-sample decimation, is
1.995 seconds; this is not a measured delay of the nonlinear Maxim algorithm. The synthetic four-point 1-Hz mean uses its exact 1.5-second
centre age. Clinical streams use **zero** projection because their processing
delays are unknown. Physiological transport delay is never estimated away.
The old 15-second `concern_trend` remains a v4 comparator only.

Sources are the checksum-pinned [dry breath-hold study](https://zenodo.org/records/21701010)
and [UW/UCSD controlled hypoxemia](https://github.com/ubicomplab/oximetry-phone-cam-data)
at commit `c483ae8fe4cd32b0b9ce412d07299b05279fc58f`.
The latter supplies six adults and four clinical oximeters; the reference for
each device is the median of the other three, not arterial blood.

| Deep-anchor variant | Warned on traces whose reference crossed 70% (16) | Warnings on traces without that crossing (8) | Median warning minus reference crossing | Latest warning |
|---|---:|---:|---:|---:|
| Existing `concern_level` | 14 | 0 | −3.5 s | +67 s |
| Existing `threshold_persist` | 16 | 0 | +4 s | +65 s |
| New `kalman_level` / `kalman_delay` | 15 | 0 | −9 s | +65 s |

The Kalman variants are identical on clinical streams because projection is
zero there. These are device-comparison timing results, not blackout
sensitivity, and the 24 device traces are clustered within six people.
The new filter does not establish a replacement for the existing firmware rule.

Both Kalman variants produced zero deep-anchor alerts in the dry breath-hold
sessions (3.979 hours) and the controls excluded from fitting (2 hours).
The two-sided Poisson upper 95% limits are about 0.93/h and 1.84/h; these are
small, inspected datasets. No deep-anchor Kalman alarms occurred in the
declared burst/noise stress tests. Unknown sensor delay still dominates
rapid synthetic desaturations. All legacy v4 evaluation rows reproduce exactly.

Alerts remain latched until acknowledgement. The optional restricted
OpenOximetry section remains off; no restricted data were used or sent to tools.

## Running and verification

On Colab, choose a CPU runtime and **Run all**. With `SAVE_TO_DRIVE=True`,
outputs go to `MyDrive/KappaBetaML/outputs/`. WildPPG's approximately 5.2 GB
runtime cache is shared by notebooks 02 and 04; set `WILDPPG_CACHE` explicitly
to retain it elsewhere. The downloader verifies pinned source ETags and byte
ranges; v5 also records local file hashes. Respect the original
[WildPPG source terms](https://github.com/eth-siplab/WildPPG/blob/main/LICENSE.md).

For local execution, `tools/run_notebook.py` extracts code without executing it:

```powershell
$env:PYTHONIOENCODING='utf-8'
python Software/GoogleColab/tools/run_notebook.py Software/GoogleColab/04_pulse_loss_detector.ipynb tmp/run04.py
$env:PULSE_DATA_ROOT='tmp/data-v4'
$env:PULSE_CACHE='tmp/data-v4/c2015'
$env:WILDPPG_CACHE='tmp/kaggle-real-full-b'
$env:PULSE_OUTPUT_DIR='tmp/pulse-v5-run'
$env:PULSE_DOWNLOAD='0'
$env:PULSE_QUICK='0'
$env:OMP_NUM_THREADS='4'
$env:MKL_NUM_THREADS='4'
$env:MPLBACKEND='Agg'
python -u tmp/run04.py
```

Use `PULSE_QUICK=1` with a separate output directory for a smoke test. It is
explicitly marked as smoke-only and never selects a firmware model. The full
split uses all 16 people; `WILDPPG_IDS` does not override v5's fixed split.
Notebook 03 uses `CONCERN_DATA_ROOT`, `CONCERN_CACHE`, `CONCERN_OUTPUT_DIR`, and
`CONCERN_DOWNLOAD`. `KB_OPENOX=0` keeps the restricted option off.

The exact 2026-11-16 local outputs are under `tmp/kaggle-qa/v6/`: `out04_full`,
`out03_v5_final`, `ecg_audit_all`, and `v4_recheck`. Only aggregate results are
copied into the repository's results folder. The notebooks are the source of
truth; the old patch builders under `tmp/notebook-build/` are scratch files.

Useful checks:

```powershell
python Software/GoogleColab/tools/test_notebook_algorithms.py --pulse Software/GoogleColab/04_pulse_loss_detector.ipynb --oxygen-export tmp/kaggle-qa/v6/out03_v5_final/replay_reference.py
python Software/GoogleColab/tools/verify_pulse_cache.py --notebook Software/GoogleColab/04_pulse_loss_detector.ipynb --cache tmp/data-v4/c2015 --output tmp/pulse-cache-check.json
./Software/ppgSoftware/test.ps1
```

The ECG audit and frozen-v4 recheck have `--help` instructions in `tools/`.
Keep joblib/PyTorch inputs local and trusted: loading joblib executes code.
Fit checkpoints use the explicit `AUDITED_CODE_SHA256` version key; they do
not automatically fingerprint edited notebook cells. After changing training
or preprocessing code, use a new cache or regenerate that version key before
running. The completed run here used the unchanged audited implementation.
The full local run took about 113 minutes, including 51 minutes to score the
complete healthy comparison recordings.

## Firmware boundary

The standalone [PPG sketch](../ppgSoftware/README.md) still contains v4
research parameters and its explicit wearer-qualification gate. The new
learned gate and CNN ensembles are not ported into that implementation.
Notebook 03 retains the original `replay` function, and its 651 golden
firmware rows remain unchanged. New experiments do not enable pump control.

The interrupted relocated wearable sketch was repaired by restoring its
missing profile/decimator files and correcting its build path/filename.
Its ESP32-S3 compile passed with the installed core 3.3.12; documented 3.3.7
was not checked in this run. The scenario helpers pass host tests but remain
unconnected to BLE and actuator tasks. Their future integration needs a
separate review of the manual-actuation contract and history kind 6 support.
Additional demo firmware and WebApp edits appeared concurrently during this
model run. They were preserved; the research run does not certify those
broader changes.

Exact v4 notebooks and superseded approaches are in [archive/](archive/README.md).
Historical prose and tables are retained in
[the archived v4 results](archive/v4-results/README.md), including the
explicitly withdrawn visibility conclusions.
