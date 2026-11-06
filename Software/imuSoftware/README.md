# IMU risk evaluator

This Arduino CLI sketch contains only the post-processing boundary for the IMU
model. It accepts a fixed-size `60 x 6` tensor with channel order
`[ax, ay, az, gx, gy, gz]`, runs a replaceable 1D-CNN inference method, and
returns `1` when the resulting unsafe score is greater than or equal to the
temporary `0.5` threshold.

The CNN architecture, weights, and inference runtime are intentionally not
defined yet. Replace `Evaluator::runCnn()` when the trained model is available.

The evaluator can be called from firmware code like this:

```cpp
ImuRisk::ImuTensor tensor = {};
const uint8_t risk = imuRiskEvaluator.evaluate(tensor);
const float score = imuRiskEvaluator.lastScore();
```

Compile this sketch with Arduino CLI using the same ESP32-S3 target currently
used by the repository's firmware build helper:

```text
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=opi,CDCOnBoot=cdc Software/imuSoftware
```
