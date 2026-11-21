#include "ImuRiskEvaluator.h"

namespace ImuRisk {

uint8_t Evaluator::evaluate(const ImuTensor& tensor) {
  lastScore_ = runCnn(tensor);
  return lastScore_ >= TEMPORARY_RISK_THRESHOLD ? 1u : 0u;
}

float Evaluator::runCnn(const ImuTensor& tensor) {
  // TODO: run the 1D CNN over all 60 samples and six channels using the
  // trained model weights. The trained model output is expected to be in [0, 1].
  (void)tensor;
  return 0.0f;
}

} // namespace ImuRisk
