# Swim IMU CNN deployment export

This bundle contains the float32 TensorFlow Lite conversion of the latest `imu_cnn_bce.keras` model and the C++ byte array used to embed it in firmware.

## Files

- `imu_cnn_bce.tflite`: model FlatBuffer for TensorFlow Lite Micro.
- `imu_cnn_model_data.cc` / `.h`: model bytes as an aligned C++ array for firmware.
- `imu_inference_config.h`: training-run window, axis, normalization, and gate-threshold constants.
- `imu_normalization.npz` / `gate_threshold.json`: original training artifacts used to generate those constants.
- `deployment_manifest.json`: model I/O, operators, size, and conversion comparison details.
- `export_deployment.py`: reproducible Keras-to-TFLite export utility.

## Inference contract

1. Accumulate 60 samples (3 seconds at 20 Hz), in this exact column order: `ax, ay, az, gx, gy, gz`.
2. Normalize each axis as `(raw_value - kImuMean[axis]) / kImuScale[axis]`.
3. Pass one float32 tensor with shape `[1, 60, 6]` to the interpreter.
4. Read the float32 sigmoid output `[1, 1]` as the model score. The saved gate threshold is `0.9981237054`; the training-run threshold artifact is authoritative for this version.

The training data used source-labeled passive-drowning episodes and a threshold-selected high-motion proxy for active-drowning windows. The score is a learned classifier output, not a calibrated probability of real-world drowning.

## Firmware integration

Include `imu_cnn_model_data.cc` and `imu_inference_config.h` in the firmware build. Run the model with TensorFlow Lite Micro and register the model's required built-in operators. The converted model uses `CONV_2D`, `EXPAND_DIMS`, `FULLY_CONNECTED`, `LOGISTIC`, `MAX_POOL_2D`, `MEAN`, and `RESHAPE`. Verify the exact resolver/operator support and tensor-arena requirement with the firmware's selected TFLite Micro version before target integration. This export is float32; it has not been quantized.

## Regenerate

Install TensorFlow and NumPy, then run:

```bash
python export_deployment.py --model imu_cnn_bce.keras --normalization imu_normalization.npz --threshold-config gate_threshold.json --out-dir .
```
