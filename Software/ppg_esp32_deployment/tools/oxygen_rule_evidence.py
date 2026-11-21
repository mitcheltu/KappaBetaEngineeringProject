"""Figures and numbers behind the firmware oxygen rule (notebook 03 `concern_level`, 80->60 anchors).

Every replay uses Software/ppgSoftware/references/oxygen_reference.py, the frozen Python reference
that the C++ OxygenConcern.h is golden-tested against (651 rows), so the plotted behaviour is the
firmware's behaviour. Nothing is fitted or tuned here. Inputs are notebook 03 v5 outputs and the
checksum-pinned UW/UCSD controlled-hypoxemia CSVs (commit c483ae8, MIT licence).

    python Software/GoogleColab/tools/oxygen_rule_evidence.py \
        --nb03 tmp/kaggle-qa/v6/out03_v5_final --uw tmp/kaggle-qa/model_a_source_cache/uw \
        --out Software/GoogleColab/results/2026-11-16/oxygen_rule
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

ROOT = Path(__file__).resolve().parents[3]
UW_MD5 = {"100001.csv": "1b14056cc6a26781df7dce95ded359ca", "100002.csv": "59ad06c0c20a212584c94fe074c8d7a4",
          "100003.csv": "51788eed39fc1990f1b378e36dad19b6", "100004.csv": "6f17c15e32b1ff52e4d3b9c320fb9a48",
          "100005.csv": "15236e2672760cc60f8b1df486d96618", "100006.csv": "04d2a34d000d04de94cc6bb72281ecb3"}
UW_DEVICES = {"SpO2 1": "3900P", "SpO2 2": "Nellcor N-600X (a)", "SpO2 4": "Nellcor N-600X (b)",
              "SpO2 5": "Masimo Radical-7"}
HOLD_S = 5  # notebook 03's definition of a crossing: a run at/below the level lasting >= 5 s

# Reference palette (dataviz skill), light surface; slots 1-3 validate for all-pairs use.
SURFACE, INK, INK2, MUTED, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#8a8984", "#e6e5e0"
BLUE, ORANGE, AQUA = "#2a78d6", "#eb6834", "#1baf7a"


def load_reference():
    path = ROOT / "Software" / "ppgSoftware" / "references" / "oxygen_reference.py"
    spec = importlib.util.spec_from_file_location("oxygen_reference", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module, path


def style():
    plt.rcParams.update({
        "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
        "axes.edgecolor": MUTED, "axes.labelcolor": INK2, "xtick.color": INK2, "ytick.color": INK2,
        "text.color": INK, "axes.titlecolor": INK, "axes.titlesize": 12, "axes.titleweight": "bold",
        "axes.titlelocation": "left", "axes.labelsize": 10, "font.size": 10, "axes.grid": True,
        "grid.color": GRID, "grid.linewidth": 0.8, "axes.axisbelow": True, "axes.spines.top": False,
        "axes.spines.right": False, "legend.frameon": False, "lines.linewidth": 2})


def footnote(fig, text):
    fig.text(0.01, 0.015, text, color=INK2, fontsize=8.5, ha="left", va="bottom", linespacing=1.4)


def first_run(mask, t, hold=HOLD_S):
    start = None
    for i, m in enumerate(mask):
        if m:
            start = i if start is None else start
            if t[i] - t[start] >= hold:
                return start
        else:
            start = None
    return None


def load_uw(folder: Path):
    data = {}
    for name, digest in UW_MD5.items():
        path = folder / name
        if hashlib.md5(path.read_bytes()).hexdigest() != digest:
            raise ValueError(f"{name}: checksum mismatch")
        frame = pd.read_csv(path)
        frame.columns = [str(c).strip().lstrip("﻿") for c in frame.columns]
        clock = pd.to_timedelta(frame[frame.columns[0]].astype(str).str.strip(), errors="coerce")
        frame, clock = frame[clock.notna()], clock[clock.notna()]
        t = (clock - clock.iloc[0]).dt.total_seconds().round().to_numpy()
        traces = {}
        for column, label in UW_DEVICES.items():
            v = pd.to_numeric(frame[column], errors="coerce").to_numpy(float)
            traces[label] = np.where((v > 0) & (v <= 100), v, np.nan)
        data[name.split(".")[0]] = (t, traces)
    return data


def consensus(traces, exclude):
    others = np.vstack([v for d, v in traces.items() if d != exclude])
    with np.errstate(all="ignore"):
        return np.where(np.isfinite(others).sum(0) >= 2, np.nanmedian(others, 0), np.nan)


def response_curve(ref, anchors):
    """For a step from 97 % to a constant reading L: seconds to alert, and the shortest dip that alerts."""
    rows = []
    for level in range(50, 81):
        def alerts(duration, tail=60):
            x = np.r_[np.full(20, 97.0), np.full(duration, float(level)), np.full(tail, 97.0)]
            warning = ref.replay(np.arange(len(x), dtype=float), x, None, anchors, "concern_level")["warning"]
            hit = np.flatnonzero(warning)
            return (hit[0] - 20) if len(hit) else None
        sustained = alerts(300, tail=0)
        shortest = next((d for d in range(1, 301) if alerts(d) is not None), None)
        rows.append({"reading": level, "seconds_to_alert_if_sustained": sustained, "shortest_dip_that_alerts_s": shortest})
    return pd.DataFrame(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--nb03", type=Path, required=True)
    parser.add_argument("--uw", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    ref, ref_path = load_reference()
    deep = ref.ANCHOR_SETS["deep_breath_hold"]
    alert_level = deep["start"] - ref.ENTER_SCORE * (deep["start"] - deep["full"])  # 70 %
    style()
    numbers = {"reference": str(ref_path.relative_to(ROOT)), "anchors": deep, "alert_level_pct": alert_level,
               "enter_score": ref.ENTER_SCORE, "median_window_s": ref.SMOOTH_S, "tau_s": ref.TAU_S,
               "confirm_s": ref.CONFIRM_S}

    # ---- Figure 1: where the anchors sit relative to the observed minima -------------------------
    audit = pd.read_csv(args.nb03 / "participant_audit.csv")
    uw = load_uw(args.uw)
    uw_min = [{"subject": s, "device": d, "minimum": float(np.nanmin(v))} for s, (_, tr) in uw.items() for d, v in tr.items()]
    uw_min = pd.DataFrame(uw_min)
    groups = [("Dry breath-hold\n(40 people)", audit[audit.group == "AG"].min_spo2.to_numpy(float)),
              ("Normal breathing\n(40 people)", audit[audit.group == "CG"].min_spo2.to_numpy(float)),
              ("Controlled hypoxemia, UW\n(6 adults x 4 oximeters)", uw_min.minimum.to_numpy(float))]
    rng = np.random.default_rng(0)
    moderate = ref.ANCHOR_SETS["moderate"]
    moderate_level = moderate["start"] - ref.ENTER_SCORE * (moderate["start"] - moderate["full"])  # 82.5 %
    lowest_ag = float(groups[0][1].min())
    fig, ax = plt.subplots(figsize=(10, 6))
    jitter = {}
    for i, (_, values) in enumerate(groups):
        jitter[i] = i + rng.uniform(-0.18, 0.18, len(values))
        ax.scatter(jitter[i], values, s=42, color=BLUE, alpha=0.75, edgecolor=SURFACE, linewidth=1.2, zorder=3)
    for y, label, dash in [(deep["start"], f"{deep['start']:.0f}%: concern starts rising", (0, (4, 3))),
                           (alert_level, f"{alert_level:.0f}%: firmware alert level (concern 0.5)", (0, (4, 3))),
                           (deep["full"], f"{deep['full']:.0f}%: concern saturates", (0, (4, 3))),
                           (moderate_level, f"{moderate_level:.1f}%: alert level of a 90->75% rule", (0, (1, 2)))]:
        ax.axhline(y, color=INK2, linewidth=1.2, linestyle=dash, zorder=2)
        ax.text(2.62, y + 0.5, label, va="bottom", ha="left", color=INK2, fontsize=8.5)
    ax.axhspan(0, alert_level, color=ORANGE, alpha=0.06, zorder=1)
    ax.set_xticks(range(3), [g[0] for g in groups])
    ax.set_xlim(-0.5, 4.3)
    ax.set_ylim(55, 101)
    ax.set_ylabel("Lowest SpO2 reading in the recording (%)")
    ax.set_title(f"The lowest healthy breath-hold ({lowest_ag:.0f}%) sits {lowest_ag - alert_level:.0f} points above "
                 f"the alert level", pad=12)
    k = int(np.argmin(groups[0][1]))
    ax.annotate(f"lowest breath-hold: {lowest_ag:.0f}%\n({lowest_ag - moderate_level:.1f} points above a 90->75% rule)",
                (jitter[0][k], lowest_ag), xytext=(0.45, 87.5), color=INK, fontsize=9,
                arrowprops=dict(arrowstyle="-", color=MUTED, lw=1))
    footnote(fig, "Breath-hold and normal-breathing data: Zenodo 21701010 (Nonin finger oximeter, not MAX30102). UW/UCSD: "
                  "clinical finger oximeters, no arterial reference.\nThe 80% start = observed 83% minimum minus an assumed 3-point "
                  "margin, so zero breath-hold alerts is by construction, not independent evidence.\nThe 60% full anchor is an "
                  "inherited engineering value. No dataset here has blackout outcomes.")
    fig.subplots_adjust(bottom=0.25, right=0.98)
    fig.savefig(args.out / "fig1_anchor_placement.png", dpi=160)
    plt.close(fig)
    numbers["breath_hold_minimum_pct"] = lowest_ag
    numbers["moderate_rule_alert_level_pct"] = moderate_level
    numbers["normal_breathing_minimum_pct"] = float(groups[1][1].min())
    numbers["uw_trace_minimum_range_pct"] = [float(uw_min.minimum.min()), float(uw_min.minimum.max())]

    # ---- Figure 2: what a reading has to do before the rule opens an alert ----------------------
    curve = response_curve(ref, deep)
    curve.to_csv(args.out / "response_curve.csv", index=False)
    alerting = curve.dropna(subset=["seconds_to_alert_if_sustained"])
    highest_alerting = int(alerting.reading.max())
    fig, ax = plt.subplots(figsize=(9, 5.4))
    shortest = int(alerting.shortest_dip_that_alerts_s.min())
    fastest = int(alerting.seconds_to_alert_if_sustained.min())
    slowest = int(alerting.seconds_to_alert_if_sustained.max())
    ax.plot(alerting.reading, alerting.shortest_dip_that_alerts_s, color=ORANGE, marker="s", markersize=5,
            label="Shortest dip that opens an alert at all", zorder=3)
    ax.plot(alerting.reading, alerting.seconds_to_alert_if_sustained, color=BLUE, marker="o", markersize=5,
            label="Time to alert if the reading stays low", zorder=4)
    ax.axvspan(highest_alerting + 0.5, 80.5, color=GRID, alpha=0.7, zorder=0)
    ax.axhspan(0, shortest - 0.5, color=GRID, alpha=0.7, zorder=0)
    ax.text((highest_alerting + 81) / 2, slowest, f"{highest_alerting + 1}% and above:\nnever alerts,\nat any duration",
            ha="center", va="top", color=INK2, fontsize=9)
    ax.text(57, (shortest - 0.5) / 2, f"dips of {shortest - 1} s or less never alert, at any level",
            ha="center", va="center", color=INK2, fontsize=9)
    for _, row in alerting[alerting.reading.isin([50, 60, 65, highest_alerting])].iterrows():
        ax.annotate(f"{row.seconds_to_alert_if_sustained:.0f} s", (row.reading, row.seconds_to_alert_if_sustained),
                    xytext=(0, 8), textcoords="offset points", ha="center", color=INK, fontsize=9)
    ax.set_xlabel("Device SpO2 reading held after a step down from 97% (%)")
    ax.set_ylabel("Seconds (1 reading per second)")
    ax.set_xlim(49.5, 80.5)
    ax.set_ylim(0, slowest + 2)
    ax.legend(loc="upper left")
    ax.set_title(f"Readings below {highest_alerting + 1}% alert in {fastest}-{slowest} s; "
                 f"dips under {shortest} s never do", pad=12)
    footnote(fig, "Step input replayed through the frozen reference that the firmware matches (median of 3, 3-s exponential "
                  "filter, 3-s confirmation).\nAt exactly 70% concern approaches 0.5 from below and never reaches it. "
                  "Maxim's R = 1 output (~80%, typical of noise) maps to concern 0.\nTimes exclude the Maxim routine's own "
                  "4-s window and any physiological or sensor lag. From 63% down the two curves overlap.")
    fig.subplots_adjust(bottom=0.22, right=0.97)
    fig.savefig(args.out / "fig2_rule_response.png", dpi=160)
    plt.close(fig)
    numbers["highest_reading_that_alerts_pct"] = highest_alerting
    numbers["seconds_to_alert"] = {int(r.reading): int(r.seconds_to_alert_if_sustained)
                                   for _, r in alerting.iterrows() if r.reading in (50, 60, 65, highest_alerting)}
    numbers["shortest_alerting_dip_s"] = int(alerting.shortest_dip_that_alerts_s.min())

    # ---- Figure 3: real desaturations, split into sensor lag and rule latency -------------------
    timing = pd.read_csv(args.nb03 / "uw_real_desaturation_timing.csv", dtype={"subject": str})
    timing = timing[(timing.anchors == "deep_breath_hold")]
    rows = []
    for subject, (t, traces) in uw.items():
        for device, reading in traces.items():
            r = consensus(traces, device)
            ref_cross = first_run(np.nan_to_num(r, nan=101) <= alert_level, t)
            own_cross = first_run(np.nan_to_num(reading, nan=101) <= alert_level, t)
            warning = np.flatnonzero(ref.replay(t, reading, None, deep, "concern_level", auto_clear=True)["warning"])
            below = np.nan_to_num(reading, nan=101) <= alert_level
            rows.append({"subject": subject, "device": device, "reference_crossing": ref_cross is not None,
                         "device_crossing_minus_reference_s": t[own_cross] - t[ref_cross] if own_cross is not None and ref_cross is not None else np.nan,
                         "warning_minus_reference_s": t[warning[0]] - t[ref_cross] if len(warning) and ref_cross is not None else np.nan,
                         "warning_minus_device_crossing_s": t[warning[0]] - t[own_cross] if len(warning) and own_cross is not None else np.nan,
                         "warned": bool(len(warning)), "device_minimum": float(np.nanmin(reading)),
                         "device_seconds_at_or_below_70": int(below.sum())})
    traces_df = pd.DataFrame(rows)
    # Consistency with notebook 03's exported table (same rule, same data).
    check = timing[timing.variant == "concern_level"].merge(traces_df, on=["subject", "device"])
    same = np.allclose(check.warning_minus_reference_crossing_s.fillna(-999), check.warning_minus_reference_s.fillna(-999))
    if not same or not (check.warned_x == check.warned_y).all():
        raise RuntimeError("Replay disagrees with notebook 03's exported timing table")
    traces_df.to_csv(args.out / "uw_trace_decomposition.csv", index=False)
    crossing = traces_df[traces_df.reference_crossing].copy()
    crossing["label"] = crossing.subject.str[-1:].radd("Adult ") + " - " + crossing.device
    crossing = crossing.sort_values(["warning_minus_reference_s", "device_crossing_minus_reference_s"], na_position="last")
    fig, ax = plt.subplots(figsize=(9.5, 7))
    y = np.arange(len(crossing))
    for yi, (_, row) in zip(y, crossing.iterrows()):
        if np.isfinite(row.device_crossing_minus_reference_s) and np.isfinite(row.warning_minus_reference_s):
            ax.plot([row.device_crossing_minus_reference_s, row.warning_minus_reference_s], [yi, yi], color=MUTED, lw=2, zorder=2)
        if np.isfinite(row.device_crossing_minus_reference_s):
            ax.scatter(row.device_crossing_minus_reference_s, yi, s=60, facecolor=SURFACE, edgecolor=BLUE, linewidth=2, zorder=3)
        if row.warned:
            ax.scatter(row.warning_minus_reference_s, yi, s=60, color=ORANGE, edgecolor=SURFACE, linewidth=1.5, zorder=4)
        else:
            ax.text(-78, yi, f"no alert: lowest reading exactly {row.device_minimum:.0f}%, "
                             f"{row.device_seconds_at_or_below_70} s at 70%", va="center", color=INK, fontsize=8.5)
    ax.axvline(0, color=INK2, lw=1.2, linestyle=(0, (4, 3)))
    ax.set_yticks(y, crossing.label, fontsize=8.5)
    ax.set_ylim(len(crossing) - 0.5, -0.5)
    ax.set_xlim(-80, 80)
    ax.set_xlabel("Seconds relative to the other three oximeters' median crossing 70% for 5 s")
    ax.scatter([], [], s=60, facecolor=SURFACE, edgecolor=BLUE, linewidth=2, label="This oximeter's own reading crosses 70%")
    ax.scatter([], [], s=60, color=ORANGE, label="Rule alert opens (concern_level)")
    ax.legend(loc="lower left", bbox_to_anchor=(0, 1.0), ncol=2, fontsize=9)
    latency = crossing.warning_minus_device_crossing_s.dropna()
    lag = crossing.device_crossing_minus_reference_s.dropna()
    ax.set_title("Early and late alerts follow the oximeter, not the rule", pad=34)
    footnote(fig, f"{len(crossing)} device traces whose reference crossed 70%, from {crossing.subject.nunique()} adults "
                  f"(UW/UCSD finger oximeters).\nGrey segment = "
                  f"rule latency after the device itself held <=70% for 5 s: median {latency.median():.0f} s "
                  f"(range {latency.min():.0f}-{latency.max():.0f}, n={len(latency)}). Device crossing vs reference spans "
                  f"{lag.min():.0f} to +{lag.max():.0f} s.\nReference = median of the other three oximeters, not arterial "
                  "blood. Not blackout timing; traces cluster within people.")
    fig.subplots_adjust(left=0.25, bottom=0.15, right=0.97, top=0.88)
    fig.savefig(args.out / "fig3_real_desaturation_timing.png", dpi=160)
    plt.close(fig)
    numbers["uw"] = {"crossing_traces": int(len(crossing)), "crossing_adults": int(crossing.subject.nunique()),
                     "warned": int(crossing.warned.sum()),
                     "non_crossing_traces": int((~traces_df.reference_crossing).sum()),
                     "non_crossing_warned": int(traces_df[~traces_df.reference_crossing].warned.sum()),
                     "rule_latency_after_device_crossing_s": {"median": float(latency.median()), "min": float(latency.min()),
                                                              "max": float(latency.max()), "n": int(len(latency))},
                     "device_minus_reference_crossing_s": {"min": float(lag.min()), "max": float(lag.max())},
                     "missed": crossing[~crossing.warned][["subject", "device", "device_minimum",
                                                           "device_seconds_at_or_below_70"]].to_dict("records")}

    # ---- Figure 4: why not shallower anchors -----------------------------------------------------
    burden = pd.read_csv(args.nb03 / "alert_burden.csv")
    burden = burden[burden.variant == "concern_level"].set_index("anchors")
    order = [("clinical_hypoxemia", "Clinical\n95 -> 85%"), ("moderate", "Moderate\n90 -> 75%"),
             ("deep_breath_hold", "Deep (firmware)\n80 -> 60%")]
    fig, ax = plt.subplots(figsize=(9, 5.2))
    width = 0.36
    for k, (group, colour, name) in enumerate([("AG", BLUE, "Dry breath-hold"), ("CG", ORANGE, "Normal breathing")]):
        xs = np.arange(len(order)) + (k - 0.5) * width
        rates = [burden.loc[a, f"alert_starts_per_hour_{group}"] for a, _ in order]
        lo = [burden.loc[a, f"rate_ci95_lo_{group}"] for a, _ in order]
        hi = [burden.loc[a, f"rate_ci95_hi_{group}"] for a, _ in order]
        ax.bar(xs, rates, width - 0.04, color=colour, label=name, zorder=3)
        ax.errorbar(xs, rates, yerr=[np.subtract(rates, lo), np.subtract(hi, rates)], fmt="none", ecolor=INK2,
                    capsize=4, lw=1.2, zorder=4)
        for x, h, (a, _) in zip(xs, hi, order):
            starts = int(burden.loc[a, f"alert_starts_{group}"])
            sessions = int(round(burden.loc[a, f"sessions_with_alert_{group}"] * 40))
            ax.text(x, h + 0.08, f"{starts} alerts\n{sessions}/40 sessions", ha="center", va="bottom", fontsize=8.5,
                    color=INK)
    ax.set_xticks(range(len(order)), [o[1] for o in order])
    ax.set_ylabel("Alert starts per hour (95% Poisson CI)")
    ax.legend(loc="upper right")
    ax.set_ylim(0, 4.8)
    ax.set_title("Only the clinical thresholds alerted on healthy breath-holds", pad=12)
    hours_ag, hours_cg = burden.loc["deep_breath_hold", "hours_AG"], burden.loc["deep_breath_hold", "hours_CG"]
    footnote(fig, f"Same rule (concern_level), three threshold sets. 40 dry breath-hold sessions ({hours_ag:.2f} h) and "
                  f"40 normal-breathing sessions ({hours_cg:.2f} h), Zenodo 21701010;\nno participant lost consciousness "
                  "(protocol fact), so every alert is false. 0 of 40 cannot separate the moderate and deep sets: see "
                  "Figure 1 for margin.\nPreviously inspected data; Poisson intervals assume independent alerts.")
    fig.subplots_adjust(bottom=0.22, right=0.97)
    fig.savefig(args.out / "fig4_anchor_alert_burden.png", dpi=160)
    plt.close(fig)
    numbers["burden"] = {a: {g: {"alert_starts": int(burden.loc[a, f"alert_starts_{g}"]),
                                 "per_hour": float(burden.loc[a, f"alert_starts_per_hour_{g}"]),
                                 "ci95_hi": float(burden.loc[a, f"rate_ci95_hi_{g}"]),
                                 "sessions_with_alert": float(burden.loc[a, f"sessions_with_alert_{g}"])}
                             for g in ("AG", "CG")} for a, _ in order}

    # ---- Variant table (README) -------------------------------------------------------------------
    alt = pd.read_csv(args.nb03 / "alert_burden.csv")
    alt = alt[alt.anchors == "deep_breath_hold"].set_index("variant")
    held = pd.read_csv(args.nb03 / "heldout_control_burden.csv")
    held = held[held.anchors == "deep_breath_hold"].set_index("variant")
    noise = pd.read_csv(args.nb03 / "noise_robustness.csv")
    noise = noise[noise.anchors == "moderate"].set_index("variant")
    table = []
    for variant in ["concern_level", "threshold", "threshold_persist", "concern_baseline", "concern_trend",
                    "deficit_dose", "kalman_level"]:
        v = timing[timing.variant == variant]
        cross = v[v.reference_crossing]
        table.append({"variant": variant, "crossing_warned_of_16": int(cross.warned.sum()),
                      "noncrossing_warned_of_8": int(v[~v.reference_crossing].warned.sum()),
                      "median_warning_minus_reference_s": float(cross.warning_minus_reference_crossing_s.median()),
                      "breath_hold_alert_starts": int(alt.loc[variant, "alert_starts_AG"]) if variant in alt.index else None,
                      "control_alert_starts": int(alt.loc[variant, "alert_starts_CG"]) if variant in alt.index
                      else int(held.loc[variant, "alert_starts"])})
    pd.DataFrame(table).to_csv(args.out / "variant_comparison_deep_anchors.csv", index=False)
    numbers["variants"] = table
    (args.out / "numbers.json").write_text(json.dumps(numbers, indent=2, default=float) + "\n", encoding="utf-8")
    print(json.dumps(numbers, indent=1, default=float))


if __name__ == "__main__":
    main()
