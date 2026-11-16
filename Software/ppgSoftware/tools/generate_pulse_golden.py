"""Generate deterministic algorithm-parity fixtures, not clinical validation data.

The processing functions are extracted from the checked-in notebook without
executing its download/training cells. Trained scores use trusted local exports.
"""
from __future__ import annotations

import argparse
import ast
import csv
import hashlib
import json
from pathlib import Path

import joblib
import numpy as np
import pandas as pd
from scipy import signal, stats
import scipy

from export_pulse_models import FEATURES


def reference(notebook):
    cells = json.loads(notebook.read_text(encoding="utf-8"))["cells"]
    source = next("".join(c["source"]) for c in cells
                  if c["cell_type"] == "code" and "def prepare(x):" in "".join(c["source"]))
    tree = ast.parse(source)
    wanted = {"prepare", "ppg_features", "rule_definitions"}
    functions = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in wanted]
    namespace = {"np": np, "pd": pd, "signal": signal, "stats": stats,
                 "FEATURES": FEATURES, "DEVICE_FS": 100, "BRADY_BEATS": 5,
                 "ASYSTOLE_GAP_S": 4.0, "BRADY_BPM": 40.0,
                 "SOS": signal.butter(3, [0.5, 8.0], fs=100, btype="band", output="sos")}
    exec(compile(ast.Module(body=functions, type_ignores=[]), str(notebook), "exec"), namespace)
    cnn_source = next("".join(c["source"]) for c in cells
                      if c["cell_type"] == "code" and "class PulseCNN(" in "".join(c["source"]))
    cnn = next(node for node in ast.walk(ast.parse(cnn_source))
               if isinstance(node, ast.ClassDef) and node.name == "PulseCNN")
    return namespace, cnn


def make_waves():
    t = np.arange(1600) / 100
    rng = np.random.default_rng(20261109)
    basic = np.sin(2 * np.pi * 1.2 * t) + 0.2 * np.sin(2 * np.pi * 2.4 * t + .4)
    waves = {
        "normal": basic,
        "brady": np.sin(2 * np.pi * .45 * t) + .1 * np.sin(2 * np.pi * .9 * t),
        "pulse_stops": basic * (t < 9),
        "flat": np.zeros_like(t),
        "noise": rng.normal(0, .5, len(t)),
        "fading": basic * np.exp(-t / 5),
        "irregular": np.sin(2 * np.pi * (t + .14 * t * np.sin(t / 2))),
        "fast": np.sin(2 * np.pi * 3.1 * t) + .15 * rng.normal(size=len(t)),
        "burst": basic + ((t > 7) & (t < 8)) * 5 * rng.normal(size=len(t)),
        "edges": np.r_[8, np.zeros(1598), -6],
    }
    return {name: (100000 + 3000 * x).astype(np.float32) for name, x in waves.items()}


def peak_cases():
    cases = {}
    a = np.zeros(128); a[10:14] = 1; a[45:48] = 1.5; a[90] = .3
    cases["plateaus_threshold"] = a
    a = np.zeros(128); a[[10, 29, 45, 80, 109]] = [1, 1.5, 2, 1, .8]
    cases["distance_before_prominence"] = a
    a = np.zeros(128); a[[10, 20, 40, 60, 80, 100]] = 1
    cases["equal_height_distance"] = a
    a = np.array([0, 1, 0, 3, 1, 2, 1, 4, 2, 3, 0], dtype=float)
    cases["nested_bases"] = a
    cases["constant"] = np.ones(128)
    a = np.zeros(128); a[0] = 3; a[-1] = 4; a[64] = .299999
    cases["endpoints_and_below_threshold"] = a
    a = np.zeros(1600)
    a[10:1010:10] = np.tile([1., 2., 2., 1.], 25)
    cases["mixed_ties_unsupported"] = a
    return cases


def write_csv(path, fields, rows):
    with path.open("w", encoding="utf-8", newline="") as out:
        writer = csv.writer(out, lineterminator="\n")
        writer.writerow(fields)
        writer.writerows(rows)


def generate(source, output, notebook):
    ns, cnn_ast = reference(notebook)
    models = joblib.load(source / "pulse_loss_feature_models.joblib")
    config = json.loads((source / "pulse_loss_configuration.json").read_text(encoding="utf-8"))
    import torch
    from torch import nn
    torch.set_num_threads(1)
    cnn_ns = {"nn": nn}
    exec(compile(ast.Module(body=[cnn_ast], type_ignores=[]), str(notebook), "exec"), cnn_ns)
    net = cnn_ns["PulseCNN"]()
    net.load_state_dict(torch.load(source / "pulse_loss_cnn_1d.pt", map_location="cpu", weights_only=True))
    net.eval()
    output.mkdir(parents=True, exist_ok=True)
    wave_rows, summary_rows, peak_rows = [], [], []
    for name, raw in make_waves().items():
        # Input quantization is identical to the float ring on the MCU.
        values = raw.astype(float)
        prepared = ns["prepare"](values)
        features, peaks = ns["ppg_features"](values)
        row = [name] + [features[key] for key in FEATURES]
        scores = {"rule_definitions": float(features["max_gap_s"] >= 4 or
                                            (features["hr_min5"] < 40 and features["n_beats"] > 0))}
        for model, clf in models.items():
            scores[model] = float(clf.predict_proba(np.array([[features[k] for k in FEATURES]]))[0, 1])
        with torch.no_grad():
            scores["cnn_1d"] = float(torch.sigmoid(net(torch.tensor(prepared[None, :], dtype=torch.float32)))[0])
        for model in ["rule_definitions", "logistic", "boosted_trees", "cnn_1d"]:
            thresholds = config["final_thresholds"].get(model, {"high_sensitivity": .5, "high_specificity": .5})
            row += [scores[model], int(scores[model] >= thresholds["high_sensitivity"]),
                    int(scores[model] >= thresholds["high_specificity"])]
        summary_rows.append(row)
        wave_rows.extend([name, i, format(float(raw[i]), ".9g"), format(prepared[i], ".17g")]
                         for i in range(len(raw)))
        if len(peaks):
            prominences = signal.peak_prominences(prepared, peaks)[0]
            peak_rows.extend([name, int(p), format(prom, ".17g")] for p, prom in zip(peaks, prominences))
    model_fields = [f"{m}_{suffix}" for m in ["rule", "logistic", "boosted", "cnn"]
                    for suffix in ["score", "high_sensitivity", "high_specificity"]]
    write_csv(output / "pulse_waveforms.csv", ["case", "sample", "raw", "prepared"], wave_rows)
    write_csv(output / "pulse_expected.csv", ["case"] + FEATURES + model_fields, summary_rows)
    write_csv(output / "pulse_peaks.csv", ["case", "sample", "prominence"], peak_rows)
    corner_rows, corner_peaks = [], []
    for name, z in peak_cases().items():
        distance = 1 if name == "nested_bases" else 30
        peaks, props = signal.find_peaks(z, distance=distance, prominence=.3)
        corner_rows.extend([name, distance, i, format(x, ".17g")] for i, x in enumerate(z))
        corner_peaks.extend([name, int(p), format(prom, ".17g")]
                            for p, prom in zip(peaks, props["prominences"]))
    write_csv(output / "pulse_peak_cases.csv", ["case", "distance", "sample", "value"], corner_rows)
    write_csv(output / "pulse_peak_expected.csv", ["case", "sample", "prominence"], corner_peaks)
    manifest = {"purpose": "Synthetic numerical parity fixtures; no clinical/outcome labels",
                "seed": 20261109, "notebook_sha256": hashlib.sha256(notebook.read_bytes()).hexdigest(),
                "numpy": np.__version__, "scipy": scipy.__version__, "torch": torch.__version__,
                "feature_absolute_tolerance": 1e-7, "score_absolute_tolerance": 1e-5,
                "unsupported_corner": "Exact equal-height conflicting peaks: NumPy argsort tie order is not portable; C++ flags ambiguity and abstains from model inference",
                "source_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                  for p in source.glob("pulse_loss_*") if p.is_file()},
                "fixture_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                   for p in output.glob("pulse_*.csv")}}
    (output / "pulse_golden_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {len(summary_rows)} waveform cases and {len(peak_cases())} peak cases to {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Directory of trusted notebook 04 artifacts")
    root = Path(__file__).resolve().parents[2]
    parser.add_argument("--output", type=Path, default=root / "tests" / "ppg")
    parser.add_argument("--notebook", type=Path, default=root / "GoogleColab" / "04_pulse_loss_detector.ipynb")
    args = parser.parse_args()
    generate(args.source, args.output, args.notebook)
