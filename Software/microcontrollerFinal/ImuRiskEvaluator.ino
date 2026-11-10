#include "ImuRiskEvaluator.h"

namespace ImuRisk {

uint8_t Evaluator::evaluate(const ImuTensor& tensor) {
  return evaluate(tensor, 0, 0);
}

uint8_t Evaluator::evaluate(const ImuTensor& tensor, uint64_t inputStartUs,
                            uint64_t inputEndUs) {
  result_.score = runCnn(tensor);
  result_.inputStartUs = inputStartUs;
  result_.inputEndUs = inputEndUs;
  result_.valid = std::isfinite(result_.score) && result_.score >= 0.0f &&
                  result_.score <= 1.0f;
  result_.classification = result_.valid && result_.score >= threshold_ ? 1u : 0u;
  return result_.classification;
}

bool Evaluator::setThreshold(float threshold) {
  if (!std::isfinite(threshold) || threshold < 0.0f || threshold > 1.0f)
    return false;
  threshold_ = threshold;
  return true;
}

float Evaluator::runCnn(const ImuTensor& tensor) {
  // TODO: run the 1D CNN over all 60 samples and six channels using the
  // trained model weights. The trained model output is expected to be in [0, 1].
  (void)tensor;
  return 0.0f;
}

} // namespace ImuRisk
