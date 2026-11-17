"""Export the teammate's IMU CNN (Software/models/swim_imu_esp32_deployment) to a plain C++ header.

The demo sketch runs the model with a hand-written forward pass (ImuCnn.h), so the Arduino
build needs no TensorFlow Lite Micro library. This script reads the weights straight from
the .tflite FlatBuffer (pip package `tflite`, no TensorFlow), checks the graph is the
expected Conv1D(6->32,k5,SAME,ReLU) -> MaxPool(2) -> Conv1D(32->64,k3,SAME,ReLU) ->
mean -> Dense(64->1) -> sigmoid, and writes:

  DrowningWearableDemo/ImuCnnParams.h   weights, normalization, gate threshold
  ../../tests/scenario/imu_cnn_expected.csv  parity fixture from the LiteRT interpreter
                                             (needs `ai-edge-litert`; skipped with --no-fixture)

Fixture inputs are deterministic synthetic windows, not dataset rows.

    python Software/testsAndDemo/scenarios/export_imu_cnn.py
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
SOFTWARE = HERE.parents[1]
BUNDLE = SOFTWARE / "models" / "swim_imu_esp32_deployment"


def tensors_and_ops(model_path: Path):
    import tflite
    buf = model_path.read_bytes()
    model = tflite.Model.GetRootAsModel(buf, 0)
    graph = model.Subgraphs(0)
    names = {v: k for k, v in tflite.BuiltinOperator.__dict__.items() if not k.startswith("_")}
    ops = []
    for i in range(graph.OperatorsLength()):
        op = graph.Operators(i)
        name = names[model.OperatorCodes(op.OpcodeIndex()).BuiltinCode()]
        options = {}
        raw = op.BuiltinOptions()
        if name == "CONV_2D":
            c = tflite.Conv2DOptions(); c.Init(raw.Bytes, raw.Pos)
            options = dict(padding=c.Padding(), stride=(c.StrideH(), c.StrideW()),
                           dilation=(c.DilationHFactor(), c.DilationWFactor()), act=c.FusedActivationFunction())
        elif name == "MAX_POOL_2D":
            c = tflite.Pool2DOptions(); c.Init(raw.Bytes, raw.Pos)
            options = dict(padding=c.Padding(), kernel=(c.FilterHeight(), c.FilterWidth()),
                           stride=(c.StrideH(), c.StrideW()), act=c.FusedActivationFunction())
        elif name == "FULLY_CONNECTED":
            c = tflite.FullyConnectedOptions(); c.Init(raw.Bytes, raw.Pos)
            options = dict(act=c.FusedActivationFunction())
        ops.append((name, [op.Inputs(j) for j in range(op.InputsLength())], options))

    def tensor(index):
        t = graph.Tensors(index)
        data = model.Buffers(t.Buffer()).DataAsNumpy()
        shape = tuple(t.ShapeAsNumpy())
        assert t.Type() == tflite.TensorType.FLOAT32, "float32 model expected"
        return np.frombuffer(np.asarray(data).tobytes(), dtype="<f4").reshape(shape)
    return ops, tensor


def array(name, values):
    values = np.asarray(values, dtype=np.float32).ravel()
    def literal(v):
        text = format(float(v), ".9g")
        return text + ("f" if any(c in text for c in ".en") else ".0f")
    rows = [", ".join(literal(v) for v in values[i:i + 8]) for i in range(0, len(values), 8)]
    return f"constexpr float {name}[{len(values)}] = {{\n  " + ",\n  ".join(rows) + "\n};\n"


def synthetic_windows():
    rng = np.random.default_rng(20261116)
    t = np.arange(60) / 20.0
    out = {"still_face_up": np.tile([0.0, 0.0, 9.81, 0.0, 0.0, 0.0], (60, 1)),
           "still_tilted": np.tile([-4.5, -2.4, 8.0, 0.0, 0.0, 0.0], (60, 1))}
    for name, f, amp in [("stroke_slow", 0.6, 6.0), ("stroke_fast", 1.4, 12.0), ("thrash", 3.0, 18.0)]:
        w = np.zeros((60, 6))
        w[:, 0] = -3 + amp * np.sin(2 * np.pi * f * t)
        w[:, 1] = -2.5 + 0.6 * amp * np.cos(2 * np.pi * f * t)
        w[:, 2] = 4 + 0.5 * amp * np.sin(2 * np.pi * 2 * f * t)
        w[:, 3:] = (amp / 5) * np.stack([np.cos(2 * np.pi * f * t + k) for k in range(3)], 1)
        out[name] = w
    for k in range(5):
        out[f"noise_{k}"] = rng.normal([-3.4, -2.5, 3.7, -0.5, 0.1, -0.1], [8.5, 7.7, 6.5, 2.9, 2.5, 2.6], (60, 6))
    return {k: v.astype(np.float32) for k, v in out.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bundle", type=Path, default=BUNDLE)
    parser.add_argument("--header", type=Path, default=HERE.parent / "DrowningWearableDemo" / "ImuCnnParams.h")
    parser.add_argument("--fixture", type=Path, default=SOFTWARE / "tests" / "scenario" / "imu_cnn_expected.csv")
    parser.add_argument("--no-fixture", action="store_true")
    args = parser.parse_args()
    model_path = args.bundle / "imu_cnn_bce.tflite"
    manifest = json.loads((args.bundle / "deployment_manifest.json").read_text(encoding="utf-8"))
    ops, tensor = tensors_and_ops(model_path)
    kinds = [name for name, _, _ in ops]
    expected = ["EXPAND_DIMS", "CONV_2D", "RESHAPE", "EXPAND_DIMS", "MAX_POOL_2D", "RESHAPE", "EXPAND_DIMS",
                "CONV_2D", "RESHAPE", "MEAN", "FULLY_CONNECTED", "LOGISTIC"]
    if kinds != expected:
        raise ValueError(f"Unexpected graph: {kinds}")
    conv1, pool, conv2, dense = ops[1], ops[4], ops[7], ops[10]
    SAME, RELU, NONE = 0, 1, 0
    for conv in (conv1, conv2):
        o = conv[2]
        if o["padding"] != SAME or o["stride"] != (1, 1) or o["dilation"] != (1, 1) or o["act"] != RELU:
            raise ValueError(f"Unsupported conv options {o}")
    if pool[2]["kernel"] != (1, 2) or pool[2]["stride"] != (1, 2) or pool[2]["act"] != NONE:
        raise ValueError("Unsupported pool")
    if dense[2]["act"] != NONE:
        raise ValueError("Unsupported dense activation")
    w1, b1 = tensor(conv1[1][1]), tensor(conv1[1][2])   # [32,1,5,6] OHWI
    w2, b2 = tensor(conv2[1][1]), tensor(conv2[1][2])   # [64,1,3,32]
    wd, bd = tensor(dense[1][1]), tensor(dense[1][2])   # [1,64], [1]
    assert w1.shape == (32, 1, 5, 6) and w2.shape == (64, 1, 3, 32) and wd.shape == (1, 64)
    mean = np.asarray(manifest["normalization_mean"], np.float32)
    scale = np.asarray(manifest["normalization_scale"], np.float32)
    threshold = float(manifest["threshold"])
    model_sha = hashlib.sha256(model_path.read_bytes()).hexdigest()

    header = ["// Generated by Software/testsAndDemo/scenarios/export_imu_cnn.py; do not edit by hand.\n",
              f"// Source: Software/models/swim_imu_esp32_deployment/imu_cnn_bce.tflite sha256 {model_sha}\n",
              "// Training data (teammate): SwimWatch, swimming-recognition-lap-counting and AKF/ZPKILP IMU sets.\n",
              "// Units: acceleration m/s^2, angular rate rad/s, 20 Hz, axis order ax ay az gx gy gz.\n",
              "#pragma once\nnamespace Imu { namespace CnnParams {\n",
              f"constexpr float GateThreshold = {threshold:.10g}f;\n",
              array("Mean", mean), array("Scale", scale),
              "// Conv1 [32 out][5 tap][6 in], Conv2 [64 out][3 tap][32 in], Dense [64].\n",
              array("Conv1Weight", w1[:, 0]), array("Conv1Bias", b1),
              array("Conv2Weight", w2[:, 0]), array("Conv2Bias", b2),
              array("DenseWeight", wd), array("DenseBias", bd),
              "} }  // namespace Imu::CnnParams\n"]
    args.header.write_text("".join(header), encoding="utf-8", newline="\n")
    print(f"Wrote {args.header} (threshold {threshold})")

    if args.no_fixture:
        return
    from ai_edge_litert.interpreter import Interpreter
    interpreter = Interpreter(model_path=str(model_path))
    interpreter.allocate_tensors()
    inp, out = interpreter.get_input_details()[0], interpreter.get_output_details()[0]
    args.fixture.parent.mkdir(parents=True, exist_ok=True)
    with args.fixture.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(["case", "score"] + [f"x{t}_{a}" for t in range(60) for a in range(6)])
        for name, window in synthetic_windows().items():
            interpreter.set_tensor(inp["index"], ((window - mean) / scale)[None].astype(np.float32))
            interpreter.invoke()
            score = float(interpreter.get_tensor(out["index"])[0, 0])
            writer.writerow([name, format(score, ".9g")] + [format(float(v), ".9g") for v in window.ravel()])
    print(f"Wrote {args.fixture}")


if __name__ == "__main__":
    main()
