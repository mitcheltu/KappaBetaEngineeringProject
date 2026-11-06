#include "ImuRiskEvaluator.h"

// The upstream IMU-processing code will pass its fixed-size tensor to this
// object after producing the 60 x 6 float window.
ImuRisk::Evaluator imuRiskEvaluator;

void setup() {
  // No sensor acquisition or actuator behavior belongs in this sketch.
}

void loop() {
  // The evaluator is called by the firmware code that receives processed IMU
  // tensors. There is intentionally no polling loop here.
}
