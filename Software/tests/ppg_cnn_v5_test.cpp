#ifdef NDEBUG
#error "These assertion tests must never be compiled with -DNDEBUG"
#endif
// Numerical parity of the cnn_v5_h C++ port against PyTorch (fixtures written by
// Software/ppgSoftware/tools/export_pulse_cnn_v5.py). Not clinical validation.
#include "../ppgSoftware/PulseLoss.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using Rows = std::vector<std::vector<std::string>>;
static Rows readCsv(const std::string& path) {
  std::ifstream file(path);
  if (!file) { std::cerr << "Cannot open " << path << '\n'; assert(false); }
  Rows result;
  std::string line;
  std::getline(file, line);
  while (std::getline(file, line)) {
    if (line.empty()) continue;
    std::stringstream stream(line);
    std::vector<std::string> fields;
    std::string field;
    while (std::getline(stream, field, ',')) fields.push_back(field);
    result.push_back(fields);
  }
  return result;
}

static void near(double actual, double expected, double tolerance, const std::string& where) {
  if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
    std::cerr.precision(12);
    std::cerr << where << ": actual=" << actual << " expected=" << expected << '\n';
    assert(false);
  }
}

int main(int argc, char** argv) {
  const std::string root = argc > 1 ? argv[1] : "Software/tests/ppg";
  static Ppg::PulseLoss processor;
  assert(Ppg::PulseLoss::available(Ppg::PulseModel::CnnV5H));
  std::map<std::string, std::vector<float>> raw;
  for (const auto& row : readCsv(root + "/pulse_waveforms.csv")) raw[row[0]].push_back(std::stof(row[2]));

  constexpr double Tolerance = 2e-5;  // float32 accumulation order differs from PyTorch
  double worst = 0;
  std::size_t cases = 0;
  for (const auto& row : readCsv(root + "/pulse_cnn_v5_expected.csv")) {
    const std::string& name = row[0];
    assert(row.size() == 7 && raw[name].size() == Ppg::PulseLoss::SampleCount);
    for (unsigned point = 0; point < 2; ++point) {
      const auto result = processor.evaluate(raw[name].data(), Ppg::PulseModel::CnnV5H,
                                             static_cast<Ppg::PulseOperatingPoint>(point));
      assert(result.valid);
      assert(result.peakCount == 0);  // the CNN path never runs peak detection
      for (std::size_t m = 0; m < 3; ++m)
        near(processor.cnnV5Member(m), std::stod(row[1 + m]), Tolerance, name + " member " + std::to_string(m));
      near(result.score, std::stod(row[4]), Tolerance, name + " ensemble");
      worst = std::max(worst, std::abs(result.score - std::stod(row[4])));
      near(result.threshold, Ppg::PulseCnnV5Params::Thresholds[point], 0, name + " threshold");
      // A score within tolerance of the threshold could legitimately flip; none of the fixtures do.
      assert(std::abs(result.score - result.threshold) > Tolerance);
      assert(result.alert == (std::stoi(row[5 + point]) != 0));
    }
    ++cases;
  }
  assert(cases >= 10);

  // The v4 models are untouched by the new enum value.
  assert(processor.evaluate(raw.begin()->second.data(), Ppg::PulseModel::Rule).valid);
  // Invalid input and operating points still abstain.
  assert(!processor.evaluate(nullptr, Ppg::PulseModel::CnnV5H).valid);
  assert(!processor.evaluate(raw.begin()->second.data(), Ppg::PulseModel::CnnV5H,
                              static_cast<Ppg::PulseOperatingPoint>(2)).valid);
  float invalid[Ppg::PulseLoss::SampleCount] = {};
  invalid[400] = std::numeric_limits<float>::quiet_NaN();
  assert(!processor.evaluate(invalid, Ppg::PulseModel::CnnV5H).valid);
  // Scale invariance of the preparation step carries through to the score.
  std::vector<float> scaled(raw["normal"]);
  for (auto& v : scaled) v = 100000.0f + (v - 100000.0f) * 0.25f;
  const double base = processor.evaluate(raw["normal"].data(), Ppg::PulseModel::CnnV5H).score;
  near(processor.evaluate(scaled.data(), Ppg::PulseModel::CnnV5H).score, base, 1e-4, "amplitude invariance");

  std::cout << "PPG cnn_v5_h parity: " << cases << " waveforms x 2 operating points, 3 members; worst |diff|="
            << worst << "; workspace=" << sizeof(Ppg::PulseLoss) << " bytes (CNN "
            << sizeof(Ppg::PulseCnnV5) << ")\n";
}
