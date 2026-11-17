"""Build the demo scenario CSVs from the public datasets (scripted demo input, NOT evidence).

Every channel of every scenario is one contiguous segment of one real recording; nothing is
synthesised, spliced or time-warped. Segments were picked by hand for a predictable demo
(see SCENARIOS below and README.md): they show the pipeline working, they do not measure it.

Sources (local copies; see README.md for licences and provenance):
  IMU   SwimWatch smartwatch set (github.com/farhantandia/Smartwatch-SwimStyle-and-Drowning-
        detection-model), one of the three IMU training sets. Rows are used at 20 Hz in the file's
        units (m/s^2, rad/s), which are the IMU CNN's training units.
  IR    WildPPG sternum IR (healthy, negated to the PLETH convention, 128 -> 100 Hz) or the
        PhysioNet/CinC 2015 PLETH channel (250 -> 100 Hz), with notebook 04's resample_poly.
  SpO2  UW/UCSD controlled-hypoxemia clinical oximeters, 1 Hz.

Output: one CSV per scenario at 100 Hz rows (t_ms, ir, ax..gz every 5th row, spo2 every 100th row)
plus scenarios.json. Then run compile_scenarios.py to regenerate the firmware header.

    python Software/testsAndDemo/scenarios/build_scenarios.py
"""
from __future__ import annotations

import csv
import hashlib
import io
import json
import zipfile
from pathlib import Path

import numpy as np
import pandas as pd
from scipy import io as sio, signal

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SWIMWATCH = ROOT / "Research/HeartRateFusion/architecture/swimwatch_source/files"
WILDPPG = ROOT / "tmp/kaggle-real-full-b"
C2015_ZIP = ROOT / "tmp/data-v4/c2015/training.zip"
UW = ROOT / "tmp/kaggle-qa/model_a_source_cache/uw"
UW_MD5 = {"100001": "1b14056cc6a26781df7dce95ded359ca", "100003": "51788eed39fc1990f1b378e36dad19b6",
          "100005": "15236e2672760cc60f8b1df486d96618", "100006": "04d2a34d000d04de94cc6bb72281ecb3"}
UW_COLUMNS = {"3900P": "SpO2 1", "Nellcor N-600X (a)": "SpO2 2", "Nellcor N-600X (b)": "SpO2 4",
              "Masimo Radical-7": "SpO2 5"}
SECONDS = 110
IR_HZ, IMU_HZ = 100, 20

SCENARIOS = [
    dict(id=1, slug="normal_swim", name="Normal swim",
         story="Front crawl with a healthy pulse and normal SpO2. Expected: no actuation.",
         imu=("crawl", "SmartWatch20210316_14-40-50_crawl.csv", 0),
         ir=("wildppg", "ssx", 1602.0),
         spo2=("100001", "Masimo Radical-7", 0)),
    dict(id=2, slug="passive_pulse_stops", name="Passive: pulse stops",
         story="Passive-drowning motion; the pulse waveform stops (true ICU asystole alarm). "
               "Expected: pulse alert, then actuation once the IMU gate is open.",
         imu=("PassiveDrowning", "SmartWatch20210316_14-53-27_PassiveDrowning.csv", 0),
         ir=("c2015", "a161l", 210.0),
         spo2=("100003", "Nellcor N-600X (b)", 0)),
    dict(id=3, slug="active_hypoxia", name="Active: hypoxia",
         story="Active-drowning struggle while SpO2 falls below 70 % (real controlled-hypoxemia trace). "
               "Expected: oxygen alert with the IMU gate open, then actuation.",
         imu=("ActiveDrowning", "SmartWatch20210316_15-21-52_ActiveDrowning.csv", 0),
         ir=("wildppg", "u7y", 3566.0),
         spo2=("100006", "Nellcor N-600X (a)", 533)),
    dict(id=4, slug="still_hypoxia_gate_check", name="Still + hypoxia (gate)",
         story="Mostly still passive-drowning recording (IMU gate rarely opens) with the same kind of "
               "SpO2 fall. Demonstrates the AND rule: oxygen alerts but the gate holds actuation. "
               "In a real drowning this would be a miss.",
         imu=("PassiveDrowning", "SmartWatch20210316_15-13-52_PassiveDrowning.csv", 0),
         ir=("wildppg", "u7y", 3566.0),
         spo2=("100005", "3900P", 659)),
]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_imu(folder, file, start_row):
    path = SWIMWATCH / folder / file
    x = pd.read_csv(path)[["ax", "ay", "az", "gx", "gy", "gz"]].to_numpy(float)
    rows = SECONDS * IMU_HZ
    if start_row + rows > len(x):
        raise ValueError(f"{file}: only {len(x)} rows")
    return x[start_row:start_row + rows], {"file": f"{folder}/{file}", "start_row": start_row,
                                           "sha256": sha256(path)}


def resample(x, fs):
    up, down = IR_HZ, fs
    g = np.gcd(up, down)
    return signal.resample_poly(np.asarray(x, float), up // g, down // g)


def load_ir(kind, record, start_s):
    pad = 5  # seconds of context on both sides, trimmed after resampling
    if kind == "wildppg":
        path = WILDPPG / f"WildPPG_Part_{record}.mat"
        s = sio.loadmat(path, variable_names=["sternum"], simplify_cells=True)["sternum"]
        fs = int(round(float(s["ppg_ir"]["fs"])))
        raw = -np.asarray(s["ppg_ir"]["v"], float)  # raw intensity -> PLETH convention (notebook 04)
        info = {"dataset": "WildPPG sternum IR, negated", "participant": record, "fs": fs}
    else:
        with zipfile.ZipFile(C2015_ZIP) as z:
            lines = z.read(f"training/{record}.hea").decode("latin1").splitlines()
            sigs = [(l.split()[-1], float(l.split()[2].split("/")[0].split("(")[0]))
                    for l in lines[1:] if l and not l.startswith("#")]
            fs = int(lines[0].split()[2])
            k = [n for n, _ in sigs].index("PLETH")
            raw = sio.loadmat(io.BytesIO(z.read(f"training/{record}.mat")))["val"][k].astype(float) / sigs[k][1]
            alarms = pd.read_csv(io.BytesIO(z.read("training/ALARMS")), header=None, names=["r", "alarm", "label"])
        row = alarms[alarms.r == record].iloc[0]
        info = {"dataset": "PhysioNet/CinC 2015 PLETH", "record": record, "fs": fs, "alarm": row.alarm,
                "expert_label_true_alarm": bool(row.label), "alarm_at_s": 300}
        path = C2015_ZIP
    a, b = int((start_s - pad) * fs), int((start_s + SECONDS + pad) * fs)
    if a < 0 or b > len(raw) or not np.isfinite(raw[a:b]).all():
        raise ValueError(f"{record}: segment unavailable or non-finite")
    x = resample(raw[a:b], fs)[pad * IR_HZ:(pad + SECONDS) * IR_HZ]
    info.update(start_s=start_s, source_sha256=sha256(path))
    return x, info


def load_spo2(subject, device, start_s):
    path = UW / f"{subject}.csv"
    if hashlib.md5(path.read_bytes()).hexdigest() != UW_MD5[subject]:
        raise ValueError(f"{subject}: checksum mismatch")
    frame = pd.read_csv(path)
    frame.columns = [str(c).strip().lstrip("﻿") for c in frame.columns]
    clock = pd.to_timedelta(frame[frame.columns[0]].astype(str).str.strip(), errors="coerce")
    frame, clock = frame[clock.notna()], clock[clock.notna()]
    t = (clock - clock.iloc[0]).dt.total_seconds().round().to_numpy()
    v = pd.to_numeric(frame[UW_COLUMNS[device]], errors="coerce").to_numpy(float)
    lookup = dict(zip(t.astype(int), v))
    values = np.array([lookup.get(start_s + k, np.nan) for k in range(SECONDS)])
    valid = np.isfinite(values) & (values > 0) & (values <= 100)
    return np.where(valid, values, np.nan), valid, {"dataset": "UW/UCSD controlled hypoxemia (c483ae8)",
                                                    "subject": subject, "device": device, "start_s": start_s}


def main():
    manifest = {"purpose": "Scripted demo playback for on-ESP32 inference. Not training, test or evidence data.",
                "seconds": SECONDS, "rates_hz": {"ir": IR_HZ, "imu": IMU_HZ, "spo2": 1},
                "units": {"ir": "arbitrary, PLETH convention (rises with blood volume)",
                          "imu": "ax ay az m/s^2, gx gy gz rad/s (IMU CNN training units)",
                          "spo2": "percent; spo2_valid=0 means estimator unavailable"},
                "scenarios": []}
    for sc in SCENARIOS:
        imu, imu_info = load_imu(*sc["imu"])
        ir, ir_info = load_ir(*sc["ir"])
        spo2, valid, spo2_info = load_spo2(*sc["spo2"])
        out = HERE / f"scenario_{sc['id']}_{sc['slug']}.csv"
        with out.open("w", encoding="utf-8", newline="") as stream:
            w = csv.writer(stream, lineterminator="\n")
            w.writerow(["t_ms", "ir", "ax", "ay", "az", "gx", "gy", "gz", "spo2", "spo2_valid"])
            for i in range(SECONDS * IR_HZ):
                t_ms = i * 10
                row = [t_ms, format(ir[i], ".7g")]
                row += [format(v, ".7g") for v in imu[t_ms // 50]] if t_ms % 50 == 0 else [""] * 6
                if t_ms % 1000 == 0:
                    s = t_ms // 1000
                    row += [format(spo2[s], ".4g") if valid[s] else "", int(valid[s])]
                else:
                    row += ["", ""]
                w.writerow(row)
        manifest["scenarios"].append({k: sc[k] for k in ("id", "slug", "name", "story")} |
                                     {"csv": out.name, "imu": imu_info, "ir": ir_info, "spo2": spo2_info})
        print(f"wrote {out.name}")
    (HERE / "scenarios.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
