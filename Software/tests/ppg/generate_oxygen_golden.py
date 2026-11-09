"""Generate golden traces from the exact notebook 03 v4 exported replay.

Run from any directory: python Software/tests/ppg/generate_oxygen_golden.py
Requires NumPy. No downloads or training are performed.
The C++ adapter also rejects out-of-range percentages and honors an explicit
validity flag; these inputs are passed to the original replay as NaN.
"""

import ast
import csv
import importlib.util
import json
import math
from pathlib import Path

import numpy as np

SOFTWARE = Path(__file__).resolve().parents[2]
REFERENCE = SOFTWARE / "ppgSoftware/references/oxygen_reference.py"
NOTEBOOK = SOFTWARE / "GoogleColab/03_oxygen_concern.ipynb"
OUTPUT = Path(__file__).with_name("oxygen_golden.csv")


def verify_notebook_reference():
    """Ensure the committed oracle's actual functions/constants match notebook 03."""
    notebook = json.loads(NOTEBOOK.read_text(encoding="utf-8"))
    code = "\n".join("".join(c["source"]) for c in notebook["cells"] if c["cell_type"] == "code")
    notebook_tree = ast.parse(code)
    reference_tree = ast.parse(REFERENCE.read_text(encoding="utf-8"))
    notebook_functions = {node.name: node for node in notebook_tree.body if isinstance(node, ast.FunctionDef)}
    for node in reference_tree.body:
        if isinstance(node, ast.FunctionDef):
            assert ast.dump(node) == ast.dump(notebook_functions[node.name]), node.name
    # Values can be exported as evaluated literals, whereas the notebook can
    # use references such as CONFIRM_S when declaring its VARIANTS dictionary.
    names = {target.id for node in reference_tree.body if isinstance(node, ast.Assign)
             for target in node.targets if isinstance(target, ast.Name)}
    constants = [node for node in notebook_tree.body if isinstance(node, ast.Assign)
                 and all(isinstance(t, (ast.Name, ast.Tuple)) for t in node.targets)
                 and all(t.id in names for target in node.targets for t in ast.walk(target)
                         if isinstance(t, ast.Name))]
    namespace = {}
    exec(compile(ast.Module(body=constants, type_ignores=[]), str(NOTEBOOK), "exec"), namespace)
    for name in names:
        assert namespace[name] == getattr(reference, name), name


spec = importlib.util.spec_from_file_location("oxygen_reference", REFERENCE)
reference = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reference)
verify_notebook_reference()

cases = []


def add(name, readings, *, times=None, valid=None, auto_clear=False, anchors="deep_breath_hold"):
    readings = np.asarray(readings, dtype=float)
    times = np.arange(len(readings), dtype=np.int64) if times is None else np.asarray(times, dtype=np.int64)
    valid = np.ones(len(readings), dtype=bool) if valid is None else np.asarray(valid, dtype=bool)
    accepted = valid & np.isfinite(readings) & (readings > 0) & (readings <= 100)
    expected = reference.replay(times, np.where(accepted, readings, np.nan), None,
                                reference.ANCHOR_SETS[anchors], "concern_level", auto_clear=auto_clear)
    cases.append((name, readings, times, valid, auto_clear, anchors, expected))


add("normal", [98] * 20)
add("initial_low", [60] * 20)
add("exact_enter", [70] * 20)
add("below_enter", [70.01] * 20)
add("onset_latched_recovery", [98] * 8 + [60] * 24 + [98] * 25)
add("onset_auto_recovery", [98] * 8 + [60] * 24 + [98] * 25, auto_clear=True)
add("single_spike", [98] * 8 + [40] + [98] * 8)
add("two_sample_start", [60, 60])
add("invalid_restarts_confirmation", [60] * 12, valid=[True] * 4 + [False] + [True] * 7)
add("invalid_keeps_latch", [60] * 8 + [float("nan"), 60, 60, 60, 98, 98, 98])
add("invalid_percentages", [98, 98, 98, 0, 60, 60, 60, 101, 60, 60, 60])
add("gap_retains_filter", [60] * 8 + [98] * 12, times=list(range(8)) + list(range(30, 42)))
add("gap_restarts_confirmation", [60] * 12, times=[0, 1, 2, 3, 15, 16, 17, 18, 19, 20, 21, 22])
add("duplicate_timestamp", [60] * 12, times=[0, 1, 2, 3, 3, 4, 5, 6, 7, 8, 9, 10])
add("backward_timestamp", [60] * 12, times=[10, 11, 12, 13, 4, 5, 6, 7, 8, 9, 10, 11])
add("uint32_rollover", [60] * 12, times=np.arange(2**32 - 4, 2**32 + 8))
add("clinical_anchors", [98] * 5 + list(np.linspace(98, 80, 40)) + [98] * 20, anchors="clinical_hypoxemia", auto_clear=True)
add("moderate_anchors", [98] * 5 + list(np.linspace(98, 70, 40)) + [98] * 20, anchors="moderate", auto_clear=True)
add("descending_ramp", list(np.linspace(100, 50, 101)) + list(np.linspace(50, 100, 101)), auto_clear=True)

with OUTPUT.open("w", newline="", encoding="utf-8") as file:
    writer = csv.writer(file, lineterminator="\n")
    writer.writerow(["case", "start", "full", "auto_clear", "second", "spo2", "valid", "concern", "active"])
    rows = 0
    for name, readings, times, valid, auto_clear, anchors, expected in cases:
        anchor = reference.ANCHOR_SETS[anchors]
        for i, oxygen in enumerate(readings):
            value = expected["concern"][i]
            writer.writerow([name, anchor["start"], anchor["full"], int(auto_clear),
                             int(times[i]) % 2**32, format(float(oxygen), ".12g"), int(valid[i]),
                             "nan" if math.isnan(value) else format(float(value), ".12g"),
                             int(expected["warning"][i])])
            rows += 1
print(f"Verified notebook reference; wrote {rows} rows across {len(cases)} cases to {OUTPUT}")
