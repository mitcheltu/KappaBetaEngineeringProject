# Archived notebooks

Superseded by the two-track design described in [../README.md](../README.md). Kept for traceability; not maintained. The older notebooks retain their original Kaggle paths; the v4 snapshots use Colab/local storage.

| Notebook | Why archived |
|---|---|
| `03_oxygen_concern_v4.ipynb` | Exact pre-Kalman snapshot. Its `concern_level` implementation remains the current standalone firmware oracle. |
| `04_pulse_loss_detector_v4.ipynb` | Exact pre-audit snapshot. Its ECG counter overcounts beats; the beat-visibility conclusions and conditioned false-alarm strata must not be reused. ICU PPG models do not use that ECG counter. |
| `01_desaturation.ipynb` | Forecast of a future ≥4-point SpO₂ fall from HR+SpO₂ on dry apnea, altitude (HAPP) and lab hypoxemia (UW) data. Learned models detected 0/4 held-out episodes (trend rule 2/4); HR added no value. Replaced by the separate HR classifier (04) and oxygen concern rule (03). |
| `03_oxygen_concern_v1_altitude_lab.ipynb` | First oxygen concern rule, tuned on altitude/lab/dry-apnea data against events defined from the same SpO₂ stream (circular). Replaced by the breath-hold version with declared, untuned constants. |
| `04_hr_syncope_risk_v3_tilt.ipynb` | Heart-rate track v1–v3: tilt-table syncope recordings (sbng.si). The public release has no outcomes or event times, so v3 ran only a label-free false-alarm sweep and trained only from an investigator/clinician label file. It was archived on 2026-11-09 because heart rate alone has no usable signature for hypoxic drowning. Replaced by `04_pulse_loss_detector.ipynb`, which trains on expert-labelled alarms (Challenge 2015). The pipeline still works if the tilt study's authors share outcomes: run it with `HRSYNC_ADJUDICATED_CSV`. Its builder is `tmp/notebook-build/build_04_hr_syncope_risk.py`. |
