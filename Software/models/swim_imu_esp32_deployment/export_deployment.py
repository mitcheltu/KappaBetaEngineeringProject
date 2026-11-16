#!/usr/bin/env python3
"""Convert the trained Keras IMU CNN to a float32 TFLite Micro C++ bundle."""
import argparse
import json
from pathlib import Path

import numpy as np
import tensorflow as tf


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--model', required=True, type=Path)
    parser.add_argument('--normalization', required=True, type=Path)
    parser.add_argument('--threshold-config', required=True, type=Path)
    parser.add_argument('--out-dir', required=True, type=Path)
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)

    model = tf.keras.models.load_model(args.model, compile=False)
    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    blob = converter.convert()  # float32 baseline; no quantization
    (args.out_dir / 'imu_cnn_bce.tflite').write_bytes(blob)

    data = np.frombuffer(blob, dtype=np.uint8)
    with (args.out_dir / 'imu_cnn_model_data.cc').open('w') as f:
        f.write('#include "imu_cnn_model_data.h"\n\n')
        f.write('alignas(16) const unsigned char g_imu_cnn_model_data[] = {\n')
        for i in range(0, len(data), 12):
            f.write('  ' + ', '.join(f'0x{b:02x}' for b in data[i:i+12]) + ',\n')
        f.write('};\n')
        f.write('const unsigned int g_imu_cnn_model_data_len = sizeof(g_imu_cnn_model_data);\n')
    (args.out_dir / 'imu_cnn_model_data.h').write_text(
        '#pragma once\n\nextern const unsigned char g_imu_cnn_model_data[];\n'
        'extern const unsigned int g_imu_cnn_model_data_len;\n')

    with np.load(args.normalization) as z:
        mean, scale = z['mean'].astype(np.float32), z['scale'].astype(np.float32)
        axes = [str(x) for x in z['axes']]
    cfg = json.loads(args.threshold_config.read_text())
    threshold = float(cfg['threshold'])
    lines = [
        '#pragma once', '', '// Generated from the paired training artifacts; preserve axis order.',
        'constexpr int kImuWindowSamples = 60;', 'constexpr int kImuAxisCount = 6;',
        'constexpr int kImuWindowStrideSamples = 20;',
        f'constexpr float kImuGateThreshold = {threshold:.10g}F;',
        'constexpr const char* kImuAxisOrder[kImuAxisCount] = {' + ', '.join(f'"{x}"' for x in axes) + '};',
        'constexpr float kImuMean[kImuAxisCount] = {' + ', '.join(f'{float(x):.9g}F' for x in mean) + '};',
        'constexpr float kImuScale[kImuAxisCount] = {' + ', '.join(f'{float(x):.9g}F' for x in scale) + '};', '']
    (args.out_dir / 'imu_inference_config.h').write_text('\n'.join(lines))

    rng = np.random.default_rng(123)
    sample = rng.normal(size=(4, 60, 6)).astype(np.float32)
    keras_out = np.asarray(model(sample, training=False)).reshape(-1)
    interpreter = tf.lite.Interpreter(model_content=blob)
    interpreter.allocate_tensors()
    inp, out = interpreter.get_input_details()[0], interpreter.get_output_details()[0]
    tflite_out = []
    for window in sample:
        interpreter.set_tensor(inp['index'], window[None, ...])
        interpreter.invoke()
        tflite_out.append(float(interpreter.get_tensor(out['index']).reshape(-1)[0]))
    ops = sorted({str(op['op_name']) for op in interpreter._get_ops_details()} - {'DELEGATE'})
    manifest = {
        'source_model': args.model.name, 'deployment_model': 'imu_cnn_bce.tflite',
        'format': 'TensorFlow Lite float32 FlatBuffer',
        'input_shape': inp['shape'].astype(int).tolist(), 'input_dtype': str(inp['dtype']),
        'output_shape': out['shape'].astype(int).tolist(), 'output_dtype': str(out['dtype']),
        'keras_tflite_max_abs_error_on_4_random_inputs': float(np.max(np.abs(keras_out - tflite_out))),
        'tflite_ops': ops, 'model_bytes': len(blob), 'threshold': threshold,
        'window_samples': 60, 'window_stride_samples': 20, 'axis_order': axes,
        'normalization_mean': mean.tolist(), 'normalization_scale': scale.tolist(),
    }
    (args.out_dir / 'deployment_manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
