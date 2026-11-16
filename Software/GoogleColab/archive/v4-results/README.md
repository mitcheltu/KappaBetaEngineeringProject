# Google Colab notebooks: two-track risk design (heart rate + oxygen)

**Audit in progress, 2026-11-16:** the v4 ECG counter overcounts beats. Its
"half the ECG beats", "13% pulse visible", and visibility-conditioned false-alarm
conclusions below are withdrawn. Notebook 04 v5 is being rerun; all 16 WildPPG
people already appear in notebook 02 results, so the new split is a development
comparison, not untouched validation. Notebook 03 now includes a fitted Kalman
comparison. See the archived v4 notebooks for the exact prior implementations.

The wearable's physiological risk is split into **two independent tracks**. Each has its own data, its own model type and its own failure modes:

| Track | Notebook | Model type | Training / evaluation data |
|---|---|---|---|
| Heart rate: pulse lost / dangerous rhythm | [04_pulse_loss_detector.ipynb](04_pulse_loss_detector.ipynb) | Rule (published alarm definitions) vs logistic, boosted trees, 1D CNN on the PPG waveform | Trained/tested on expert-labelled ICU alarms (Challenge 2015, finger PPG); false alarms on healthy sternum PPG (WildPPG) |
| Heart-rate signal quality (`q`) | [02_hr_reliability.ipynb](02_hr_reliability.ipynb) | Learned gate: is the optical HR within 5 bpm of ECG? | WildPPG sternum PPG + ECG (land activities) |
| Oxygen concern | [03_oxygen_concern.ipynb](03_oxygen_concern.ipynb) | Deterministic rule with fixed constants (no outcome training) | Dry breath-holds vs normal breathing; **real** controlled desaturations to 61–77% (UW/UCSD, four clinical oximeters); synthetic desaturations through a sensor-delay model; optional OpenOximetry arterial reference |

The notebooks are independent. Every source is checksum-pinned, and no API keys are needed. **None of them is a validated blackout detector**, and every exported configuration is marked `deployable: false`.

**Label policy (v3, 2026-11-09):** no notebook trains or scores against labels it invented. The only labels allowed are:
- protocol facts, such as study group, or the fact that nobody lost consciousness;
- outcomes determined by the original investigators or a clinician, with the source stated.

Automatic detections are review candidates only.

## Running on Google Colab

1. **Open a notebook.** Go to [colab.research.google.com](https://colab.research.google.com) and choose **File → Upload notebook**, or open it from GitHub or Drive. All three run on the default **CPU** runtime. A GPU gives no benefit: the models are scikit-learn or a 3,600-parameter PyTorch network, and the rules are plain NumPy.
2. **Choose Runtime → Run all.** When prompted, authorise **Google Drive**.
3. **Find the outputs** in `MyDrive/KappaBetaML/outputs/<notebook>/`, including a `figures/` subfolder. Downloads for 03/04 are cached in `MyDrive/KappaBetaML/cache/` so later sessions skip them.
   - Set `SAVE_TO_DRIVE = False` (in the first code cell, or the environment variable `KB_SAVE_TO_DRIVE=0`) to keep everything in the runtime. Colab wipes `/content` when the runtime disconnects.
4. **To skip downloads,** upload the unchanged original files to `MyDrive/KappaBetaML/input/` and set `ALLOW_DOWNLOAD=False`. Any subfolder there is searched.
5. **Resources:**

| Notebook | Runtime on free Colab |
|---|---|
| 03 | Under a minute |
| 04 | About 10 min on the first run (Challenge 2015 is 339 MB and its windows are cached), plus WildPPG: about 300 MB per participant, shared with 02 |
| 02 | Downloads 5.2 GB to the runtime disk; budget for a long session or use the subset settings |

**Driving Colab from Claude Code.** The project registers Google's official [Colab MCP server](https://github.com/googlecolab/colab-mcp) in `../../.mcp.json`; it runs through `uvx` and needs `uv`. After Claude Code restarts and you approve the server, it can open a Colab notebook in your browser, execute cells and read outputs. The browser tab must stay open and connected, and the client must run on your machine. Execution still happens on Colab's runtime.

`archive/` holds superseded notebooks, kept for traceability:
- the former 01 (future-SpO₂-drop forecaster);
- the first version of 03 (altitude/lab tuning);
- the tilt-table heart-rate notebook (`04_hr_syncope_risk_v3_tilt.ipynb`). It is reusable if the tilt study's authors share outcomes.

## 04 · Heart-rate track: pulse lost / dangerous rhythm (v4)

**Why this target.** Heart-rate *trends* have no usable signature for the common, hypoxic kind of drowning: the diving reflex slows the heart in safe breath-holds too, and the heart keeps beating for minutes after blackout. Heart rate matters when the **heart itself fails**, as in long-QT or cold-shock arrhythmias. Then the pulse disappears or becomes chaotic within seconds, so the track detects that from the PPG waveform.

**Training and test data: [PhysioNet/CinC Challenge 2015](https://physionet.org/content/challenge-2015/1.0.0/)** (open, ODC-BY; `training.zip` MD5 `99a51fefcd72b584a8122ebd7f78dacf`)
- **Labels:** expert annotators labelled each ICU monitor alarm true or false (two-thirds agreement required). We select records but never relabel.
- **Used:** asystole, extreme bradycardia, ventricular tachycardia and VF records that include PPG: **514 records, 123 true events.**

  | Alarm | True | False |
  |---|---|---|
  | Asystole | 17 | 89 |
  | Bradycardia | 39 | 38 |
  | VT | 61 | 223 |
  | VF | 6 | 41 |

- **Excluded:** extreme tachycardia (HR > 140 is normal while swimming).
- **Input:** 16 s of finger PPG ending at the alarm, resampled 250 → 100 Hz (the wearable's rate), band-passed 0.5–8 Hz.

**Architectures**

| Name | What it is | Tuned on data? |
|---|---|---|
| `rule_definitions` | No pulse for ≥ 4 s, or 5-beat HR < 40 bpm: the Challenge's own alarm definitions applied to PPG beats | No |
| `logistic` | Logistic regression on 10 PPG features (longest pulse gap, time since last pulse, beat count, median and minimum HR, irregularity, pulse fading, spectral purity, flatness, spikiness) | Yes |
| `boosted_trees` | Gradient-boosted trees on the same features | Yes |
| `cnn_1d` | 1D CNN on the raw waveform, 3,601 parameters (PyTorch; skipped if unavailable) | Yes |

All of them are small enough for the ESP32-S3.

**Evaluation**
- **Cross-validation:** outer 5-fold, stratified by alarm × label. It is record-level, because the Challenge publishes no patient IDs.
- **Operating points,** both chosen from inner, training-only cross-validation and declared in advance:
  - `high_sensitivity`: inner sensitivity ≥ 0.90;
  - `high_specificity`: inner specificity ≥ 0.90.
- **Reported:** AUROC (bootstrap CI), sensitivity and specificity (Wilson CIs), per-alarm results, and the Challenge score.
- **False-alarm test:** the frozen detectors run on **WildPPG healthy sternum infrared PPG**, a 16-s window every 2 s. Every alert counts as false.
  - Results are split by chest motion.
  - They are also split by **pulse visibility**: PPG beats ÷ beats in the simultaneous chest ECG, with 0.8–1.2 counting as visible. This separates "detector wrong" from "PPG doesn't show the pulse".

## 03 · Oxygen concern rule (v4)

**Data.** [Dry intermittent low-lung-volume apnoea](https://zenodo.org/records/21701010), `Supp database.xlsx` (MD5 `79fc9abbd6e3e04c52d17df3247751ec`, CC BY 4.0).
- 40 people performed repeated breath-holds (AG) and 40 breathed normally (CG).
- 6 minutes at 1 Hz with a Nonin finger oximeter, plus each person's resting SpO₂.
- Lowest SpO₂ is 83%. Nobody lost consciousness.

**Design.** Nothing is trained on outcomes. Constants are fixed in the first cell; they are not pre-registered, because earlier versions ran on the same data.
- **Anchor sets:** `clinical_hypoxemia` 95→85%, `moderate` 90→75%, `deep_breath_hold` 80→60% (primary).
- **Concern index:** 0–1, with an exponential filter, hysteresis (enter 0.5, exit 0.25), 3 s confirmation and 5 s recovery.

**v4 (2026-11-09): real deep desaturations.**
- **Evaluation 5** replays the rule on the open [UW/UCSD controlled-hypoxemia data](https://github.com/ubicomplab/oximetry-phone-cam-data) (MIT, commit `c483ae8`): 6 adults breathing low-oxygen gas, with four clinical finger oximeters at 1 Hz reaching **61–77%**.
  - The rule runs on one oximeter.
  - Its timing reference is the per-second median of the **other three** (independent instruments, not arterial blood).
- **OpenOximetry:** an optional cell (`KB_OPENOX=1`) downloads only its tables (about 21 MB) with the `physionet` CLI and a hidden password prompt, then prints the schema. The arterial-reference analysis comes next.

**v3 changes**
- **Person-relative branch fixed.** The anchor set is shifted by (resting SpO₂ − 97%) and combined with the absolute map by max(). In v2, a fixed 4→15-point drop made the "deep" baseline/trend variants alarm at about 88% SpO₂.
- **Latched alarms.** On the device (`auto_clear=False`) an alert stays on until acknowledged. The evaluations count alert starts with `auto_clear=True`.
- **Sensor-delay model.** Synthetic trajectories are arterial values passed through a 0/10/20/40-s delay plus the Maxim 4-s window. Timing is measured against the *arterial* crossing. The chest MAX30102's real delay is unmeasured, so this is a sweep.
- **Burst-artefact stress test.** Motion drives the red/IR ratio towards 1. The firmware's Maxim calibration (`spo2_algorithm.cpp:201`) maps that to **80.1%**.
- **Exact Poisson 95% intervals** on alert rates.

## 02 · HR signal-quality gate

This notebook is unchanged apart from its description of the firmware. It replays the firmware's SparkFun/Maxim HR routine on WildPPG sternum PPG and learns whether each 4-s reading is within 5 bpm of ECG.

The firmware now samples the MAX30102 at **100 Hz raw** (on-chip averaging off) and records every sample. It averages groups of four raw samples in software before the Maxim routine, so the routine still receives the same 25 Hz input the replay models.

The inspected run found that only 12% of Maxim readings were within 5 bpm, and that the selected gate still accepted 66% wrong readings. **The device HR input to notebook 04 is therefore currently unreliable.** WildPPG is about 5.2 GB; see the notebook header for the subset/quick settings. Licence: CC BY-NC-SA 4.0 for data, with an unresolved prepublication note. Keep it private.

## Latest local run (2026-11-09, real data, v4)

These are development results. The oxygen constants were fixed after earlier versions had been seen on the dry breath-hold data, so independent claims need new data.

**04 · Pulse lost / dangerous rhythm.** Cross-validation on ICU finger PPG, 514 records:

| Detector | AUROC | Sensitivity | Specificity |
|---|---|---|---|
| `rule_definitions` (untuned) | — | 0.27 (0.20–0.35) | 0.89 (0.86–0.92) |
| `logistic`, high-sensitivity point | 0.69 (0.64–0.75) | 0.90 | 0.24 |
| `boosted_trees`, high-sensitivity point | **0.79 (0.74–0.83)** | 0.91 | 0.43 |
| `boosted_trees`, high-specificity point | 0.79 | 0.38 | 0.91 |
| `cnn_1d`, high-specificity point | 0.69 (0.64–0.75) | 0.41 | 0.89 |

- **Per alarm type:** PPG detects bradycardia best (boosted trees catch 38/39 at the high-sensitivity point while flagging 13/38 false alarms). True VT is the hardest; many VTs still produce a pulse.
- **There are only 17 true asystole and 6 true VF events with PPG,** so those per-type results are anecdotal.

**False alarms on healthy sternum PPG.** These are 8 local 10-minute WildPPG clips (1.3 h); Colab runs the full recordings. Read the flagged-window percentage as well as alert starts per hour, because a detector stuck "on" produces few starts.

| Detector | Alert starts/h (upper 95%) | Healthy windows flagged | Flagged when PPG shows the pulse | Flagged when it doesn't |
|---|---|---|---|---|
| `rule_definitions` | 97 [115] | 30% | **0%** | 34% |
| `boosted_trees`, high-specificity point | 237 [265] | 30% | 17% | 32% |
| `cnn_1d`, high-specificity point | 27 [37] | 3% | 0% | 3% |
| Any model, high-sensitivity point | 16–131 | 89–99% | 79–99% | 89–99% |

- **The chest PPG showed only about half the ECG beats** (median ratio 0.53), and only 13% of windows showed the pulse properly. This matches notebook 02.
- When the pulse *is* visible, the untuned rule never fired.
- **The bottleneck is pulse visibility in chest PPG, not the model.** A pulse-lost alarm is meaningless unless a signal-quality gate (notebook 02) first confirms the PPG normally shows this wearer's pulse. Without that, "no pulse seen" is this signal's normal state.

**03 · Oxygen rule**
- **Real breath-holds (AG):**
  - Clinical anchors: 2.0–10 alerts/h.
  - Deep anchors: 0 alerts in 2.9 h (upper 95% limit 0.93/h).
  - Unchanged from v3; the v4 rerun reproduced every v3 table exactly.
- **Real controlled desaturations (UW/UCSD, Evaluation 5):**
  - **Device timing:** the four clinical oximeters differ by up to about 17 s (−12.5 to +4.5 s relative to the median of the others).
  - **Device spread:** about 1–3 points at low saturation, with occasional single readings 20–30 points off.
  - **Deep anchors:** in 16 device traces the other three oximeters fell below 70%. `threshold_persist` warned on all 16, a median 4 s after them (worst 65 s). `concern_level` missed 2 of 16 and `concern_baseline` missed 1. `deficit_dose` warned long before the crossing, and in 4 of 8 traces where the others never crossed.
  - **Pulse rate** rose from a median 58 bpm (SpO₂ ≥ 95%) to 77 bpm (< 80%). Breathing low-oxygen gas speeds the heart, whereas a breath-hold slows it, so heart-rate rules cannot transfer between the two.
- **Burst artefact** at the device's 80.1% point:
  - clinical anchors: 12–19 alerts/h;
  - deep `concern_trend`: 12.4/h;
  - other deep variants: 0.
- **Synthetic deep desaturations:** with a sensor delay of 20 s or more, every variant warns after arterial saturation reaches 60% for falls of at least 1 %/s.

## Sampling rates

| Signal / source | Rate | Used by |
|---|---|---|
| Wearable raw PPG (MAX30102 red/IR) | **100 Hz**, on-chip averaging off, every sample recorded | Beat timing (future); decimated 4:1 to 25 Hz for the Maxim HR/SpO₂ routine |
| Wearable HR, SpO₂ | 1 Hz (4-s Maxim window, recomputed every second) | 03, 04 inputs |
| Wearable IMU / depth | 50 Hz / 5 Hz | Not used by these notebooks |
| Challenge 2015 ICU finger PPG (04) | 250 Hz → resampled to 100 Hz | Pulse-lost detector training and testing |
| WildPPG sternum PPG + ECG (02, 04) | 128 Hz | 02: boxcar-binned to 25 Hz for the Maxim replay; 04: resampled to 100 Hz for false alarms |
| UW/UCSD clinical oximeters SpO₂ and pulse rate (03) | 1 Hz | Real-desaturation timing |
| Dry apnoea SpO₂ (Nonin finger) and HR (Polar H10) (03) | 1 Hz | Rule replay, false alarms |

**Why 100 Hz PPG.**
- **HR alone** needs only a few Hz in principle (Béres et al., *Meas Sci Rev* 2019); 25 Hz is ample.
- **SpO₂** is a ratio of pulse amplitudes, and 25 Hz is what the Maxim routine assumes. No peer-reviewed study shows a higher rate helps SpO₂ itself.
- **Beat timing** (pulse-rate variability, pauses, arrhythmia, a "pulse lost" detector) is what needs the rate.
  - Without interpolation, rounding a beat to the nearest sample adds about 1/(fs·√12) of error per beat, and about √2 times that to each beat-to-beat difference.
  - At 25 Hz that is about 16 ms per difference, comparable to a resting RMSSD of 20–50 ms. At 100 Hz it is about 4 ms.
  - Published thresholds: Choi & Shin (*Physiol Meas* 2017) found no difference from ECG above 25 Hz with interpolation, and degradation at ≤ 15 Hz. Béres et al. 2019 needed 50 Hz for SDNN and 100 Hz for RMSSD without interpolation.
- **100 Hz** covers beat timing even without interpolation, keeps the Maxim input exact (4:1), and costs about 160 history records/s, roughly 13.7 min in the 4 MiB ring.
- **Going above 100 Hz** buys little: underwater motion artefact, not sampling, limits PPG timing. Upgrade only if a measured PRV benchmark shows 100 Hz is the bottleneck.
- **What the firmware cannot fix by sampling faster:** the Maxim SpO₂ calibration is a generic curve, uncalibrated for a chest reflectance site. Below about 90% its readings are unknown until checked against a reference in a controlled-desaturation study.

1 Hz is adequate for SpO₂ trends and HR level/trend features. Match the device's averaging and delay to the dataset's when transferring thresholds. Finger oximeters lag chest/forehead sites.

## Candidate datasets for defensible labels (checked 2026-11-09)

No open dataset has real, investigator-timed loss of consciousness or syncope with HR/PPG. Hyperbaric chambers raise inspired oxygen, so they don't produce hypoxic blackout; the altitude (hypobaric) case is the relevant one.

| Dataset | Access | What it offers | Use |
|---|---|---|---|
| [PhysioNet/CinC Challenge 2015](https://physionet.org/content/challenge-2015/1.0.0/) | Open, ODC-BY | 750 ICU alarms with expert true/false labels; ECG + PPG at 250 Hz | **In use (04):** pulse-lost detector training and testing |
| [UW/UCSD controlled hypoxemia](https://github.com/ubicomplab/oximetry-phone-cam-data) | Open, MIT | 6 adults, four clinical oximeters at 1 Hz, 61–77% nadirs | **In use (03):** real-desaturation timing |
| Tilt outcomes from Klemenc/Bosnić | Email the authors | Per-patient HUTT outcome for the archived tilt notebook | Only if the vasovagal question returns |
| [OpenOximetry](https://physionet.org/content/openox-repo/) (UCSF Hypoxia Lab) | PhysioNet credentialed, DUA | Arterial co-oximetry SaO₂ reference, 70–100% targets; 2 Hz reference oximeters; some raw red/IR PPG at 86 Hz (not time-synchronised) | **Oxygen track:** check readings against arterial SaO₂ and estimate oximeter delay |
| [Vikne et al. 2025](https://osf.io/fmdk8/) (OSF) | Open; licence field blank | Normobaric hypoxia to arterial SaO₂ about 55%, 4 oximeters at 1 Hz, blood-gas reference | Oxygen track: accuracy below 70% (summary-level data) |
| [Harespod](https://pmc.ncbi.nlm.nih.gov/articles/PMC10899206/) (figshare, CC0) | Open | Hypobaric chamber at 2–4 km, finger PPG at 100 Hz, SpO₂ 1 Hz | Raw-PPG behaviour under mild hypoxia; no reference SaO₂, no outcomes |
| [HAPP](https://www.nature.com/articles/s41597-025-06508-1) (figshare, CC BY 4.0) | Open | Hypobaric chamber + flight simulator, SpO₂/HR 1 Hz | Mild-hypoxia false-alarm data |

## Outputs to inspect

| Notebook | Tables | Figures |
|---|---|---|
| 04 | `cv_results.csv` (AUROC, sensitivity/specificity with CIs), `cv_per_alarm_type.csv`, `healthy_false_alarms.csv` (by motion and pulse visibility), `fold_thresholds.csv`, `oof_predictions.csv`, models (`.joblib`, `.pt`) | Example windows, sensitivity vs specificity, healthy false alarms |
| 03 | `alert_burden.csv` (with 95% CIs), `noise_robustness.csv` (incl. burst artefact), `synthetic_behaviour.csv` (per sensor delay), `uw_device_agreement.csv`, `uw_real_desaturation_timing.csv`, `uw_pulse_by_spo2.csv`, `replay_reference.py` | Breath-hold support, alert burden, session replay, synthetic timing, real desaturations |
| 02 | `test_gates.csv`, `test_participants.csv`, `test_reference_hr_bands.csv`, `window_eligibility.csv` | Gate trade-offs, agreement/error, HR-band support |

## Troubleshooting

- **Checksum failure:** do not bypass it. The source changed and needs review.
- **No download / no Internet:** upload the unpacked originals, with their original filenames, to `MyDrive/KappaBetaML/input/` and set `ALLOW_DOWNLOAD=False`. Notebook 04 also accepts the tilt zip under any filename if its MD5 matches.
- **Missing package:** install only the package named by the preflight check, restart, and Run All.
- **Notebook 04 is slow:** the Challenge 2015 windows are cached as `c2015_ppg_windows.npz` under `MyDrive/KappaBetaML/cache/pulse_loss`. WildPPG downloads about 300 MB per participant; set `WILDPPG_IDS=an0,e61,...` to use fewer. Without PyTorch, `cnn_1d` is skipped.
- **Drive authorisation prompt blocks Run all:** accept it, or set `SAVE_TO_DRIVE = False`.
