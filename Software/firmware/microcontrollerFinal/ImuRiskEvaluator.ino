#include "ImuRiskEvaluator.h"

#include <new>

#include "imuDeployCNN/imu_cnn_model_data.h"
#include "imuDeployCNN/imu_inference_config.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace ImuRisk {
namespace {

constexpr size_t kRequiredArenaAlignment = 16;
constexpr int kInputRank = 3;
constexpr int kOutputRank = 2;

struct RuntimeState {
  tflite::MicroMutableOpResolver<7> resolver;
  alignas(tflite::MicroInterpreter)
      uint8_t interpreterStorage[sizeof(tflite::MicroInterpreter)]{};
  tflite::MicroInterpreter* interpreter = nullptr;
  TfLiteTensor* input = nullptr;
  TfLiteTensor* output = nullptr;
  void* arena = nullptr;
  size_t arenaBytes = 0;
  bool registrationAttempted = false;
  bool resolverReady = false;
  bool initialized = false;
};

RuntimeState& runtime() {
  static RuntimeState state;
  return state;
}

bool hasShape(const TfLiteTensor* tensor, const int* expected, int rank) {
  if (!tensor || !tensor->dims || tensor->dims->size != rank) return false;
  for (int i = 0; i < rank; ++i) {
    if (tensor->dims->data[i] != expected[i]) return false;
  }
  return true;
}

bool registerModelOperators(RuntimeState& state) {
  if (state.resolverReady) return true;
  if (state.registrationAttempted) return false;
  state.registrationAttempted = true;

  if (state.resolver.AddConv2D() != kTfLiteOk ||
      state.resolver.AddExpandDims() != kTfLiteOk ||
      state.resolver.AddFullyConnected() != kTfLiteOk ||
      state.resolver.AddLogistic() != kTfLiteOk ||
      state.resolver.AddMaxPool2D() != kTfLiteOk ||
      state.resolver.AddMean() != kTfLiteOk ||
      state.resolver.AddReshape() != kTfLiteOk) {
    return false;
  }
  state.resolverReady = true;
  return true;
}

}  // namespace

static_assert(kImuWindowSamples == Config::IMU_WINDOW_SAMPLES,
              "Model window must match the firmware IMU window.");
static_assert(kImuAxisCount == IMU_CHANNELS,
              "Model axis count must match the firmware IMU tensor.");

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

void Evaluator::invalidate(uint64_t inputStartUs, uint64_t inputEndUs) {
  result_.score = NAN;
  result_.inputStartUs = inputStartUs;
  result_.inputEndUs = inputEndUs;
  result_.classification = 0;
  result_.valid = false;
}

bool Evaluator::setThreshold(float threshold) {
  if (!std::isfinite(threshold) || threshold < 0.0f || threshold > 1.0f)
    return false;
  threshold_ = threshold;
  return true;
}

bool Evaluator::begin(void* tensorArena, size_t tensorArenaBytes) {
  ready_ = false;
  RuntimeState& state = runtime();

  if (state.initialized) {
    ready_ = state.arena == tensorArena && state.arenaBytes == tensorArenaBytes;
    return ready_;
  }

  if (!tensorArena || tensorArenaBytes == 0 ||
      (reinterpret_cast<uintptr_t>(tensorArena) % kRequiredArenaAlignment) != 0 ||
      !registerModelOperators(state)) {
    return false;
  }

  const tflite::Model* model = tflite::GetModel(g_imu_cnn_model_data);
  if (!model || model->version() != TFLITE_SCHEMA_VERSION) return false;

  state.arena = tensorArena;
  state.arenaBytes = tensorArenaBytes;
  state.interpreter = new (state.interpreterStorage) tflite::MicroInterpreter(
      model, state.resolver, static_cast<uint8_t*>(tensorArena), tensorArenaBytes);
  if (state.interpreter->initialization_status() != kTfLiteOk ||
      state.interpreter->AllocateTensors() != kTfLiteOk) {
    state.interpreter->~MicroInterpreter();
    state.interpreter = nullptr;
    state.arena = nullptr;
    state.arenaBytes = 0;
    return false;
  }

  state.input = state.interpreter->input(0);
  state.output = state.interpreter->output(0);
  constexpr int expectedInput[kInputRank] = {
      1, kImuWindowSamples, kImuAxisCount};
  constexpr int expectedOutput[kOutputRank] = {1, 1};
  if (state.interpreter->inputs_size() != 1 ||
      state.interpreter->outputs_size() != 1 || !state.input || !state.output ||
      !state.input->data.f || !state.output->data.f ||
      state.input->type != kTfLiteFloat32 ||
      state.output->type != kTfLiteFloat32 ||
      !hasShape(state.input, expectedInput, kInputRank) ||
      !hasShape(state.output, expectedOutput, kOutputRank) ||
      state.input->bytes < sizeof(float) * IMU_WINDOW_SAMPLES * IMU_CHANNELS ||
      state.output->bytes < sizeof(float)) {
    state.interpreter->~MicroInterpreter();
    state.interpreter = nullptr;
    state.input = nullptr;
    state.output = nullptr;
    state.arena = nullptr;
    state.arenaBytes = 0;
    return false;
  }

  state.initialized = true;
  ready_ = true;
  return true;
}

float Evaluator::runCnn(const ImuTensor& tensor) {
  RuntimeState& state = runtime();
  if (!ready_ || !state.initialized || !state.input || !state.output ||
      !state.interpreter) {
    return NAN;
  }

  float* input = state.input->data.f;
  for (size_t sample = 0; sample < IMU_WINDOW_SAMPLES; ++sample) {
    for (size_t axis = 0; axis < IMU_CHANNELS; ++axis) {
      const float rawValue = tensor[sample][axis];
      if (!std::isfinite(rawValue) || !std::isfinite(kImuScale[axis]) ||
          kImuScale[axis] <= 0.0f) {
        return NAN;
      }
      input[sample * IMU_CHANNELS + axis] =
          (rawValue - kImuMean[axis]) / kImuScale[axis];
    }
  }

  if (state.interpreter->Invoke() != kTfLiteOk) return NAN;
  return state.output->data.f[0];
}

} // namespace ImuRisk
