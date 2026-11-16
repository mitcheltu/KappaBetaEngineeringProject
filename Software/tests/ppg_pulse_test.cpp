#ifdef NDEBUG
#error "These assertion tests must never be compiled with -DNDEBUG"
#endif
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
    std::cerr.precision(17);
    std::cerr << where << ": actual=" << actual << " expected=" << expected
              << " difference=" << actual - expected << '\n';
    assert(false);
  }
}

struct Peak { std::size_t sample; double prominence; };
static std::map<std::string, std::vector<Peak>> readPeaks(const std::string& path) {
  std::map<std::string, std::vector<Peak>> peaks;
  for (const auto& row : readCsv(path))
    peaks[row[0]].push_back({std::stoul(row[1]), std::stod(row[2])});
  return peaks;
}

int main(int argc, char** argv) {
  const std::string root = argc > 1 ? argv[1] : "Software/tests/ppg";
  static Ppg::PulseLoss processor;
  std::map<std::string, std::vector<float>> raw;
  std::map<std::string, std::vector<double>> prepared;
  for (const auto& row : readCsv(root + "/pulse_waveforms.csv")) {
    assert(raw[row[0]].size() == std::stoul(row[1]));
    raw[row[0]].push_back(std::stof(row[2]));
    prepared[row[0]].push_back(std::stod(row[3]));
  }
  auto expectedPeaks = readPeaks(root + "/pulse_peaks.csv");
  std::size_t cases = 0;
  for (const auto& row : readCsv(root + "/pulse_expected.csv")) {
    const std::string& name = row[0];
    assert(row.size() == 23);
    assert(raw[name].size() == Ppg::PulseLoss::SampleCount);
    for (unsigned model = 0; model < 4; ++model) {
      for (unsigned point = 0; point < 2; ++point) {
        const auto result = processor.evaluate(raw[name].data(), static_cast<Ppg::PulseModel>(model),
                                               static_cast<Ppg::PulseOperatingPoint>(point));
        assert(result.valid);
        assert(!result.ambiguousPeakTies);
        for (std::size_t f = 0; f < 10; ++f)
          near(result.features[f], std::stod(row[1 + f]), 1e-7, name + " feature " + std::to_string(f));
        near(result.score, std::stod(row[11 + model * 3]), model == 3 ? 1e-5 : 1e-8,
             name + " score " + std::to_string(model));
        assert(result.alert == (std::stoi(row[12 + model * 3 + point]) != 0));
        assert(result.peakCount == expectedPeaks[name].size());
        for (std::size_t p = 0; p < result.peakCount; ++p) {
          assert(processor.peakIndex(p) == expectedPeaks[name][p].sample);
          near(processor.peakProminence(p), expectedPeaks[name][p].prominence, 1e-8, name + " prominence");
        }
        if (model == 0 && point == 0) {
          for (std::size_t i = 0; i < Ppg::PulseLoss::SampleCount; ++i)
            near(processor.prepared()[i], prepared[name][i], 1e-8, name + " prepared " + std::to_string(i));
        }
      }
    }
    ++cases;
  }
  // NumPy's unstable priority argsort can select a different subset for mixed
  // equal-height peaks. The helper has a deterministic rightmost contract and
  // flags this unsupported case; evaluate() abstains before any model score.
  double ambiguous[Ppg::PulseLoss::SampleCount] = {};
  for (std::size_t i = 0; i < 100; ++i)
    ambiguous[10 + i * 10] = (i % 4 == 1 || i % 4 == 2) ? 2 : 1;
  processor.detectPeaks(ambiguous, Ppg::PulseLoss::SampleCount);
  assert(processor.ambiguousPeakTies());
  double distinct[64] = {};
  distinct[10] = 1;
  distinct[20] = std::nextafter(1.0, 2.0);
  assert(processor.detectPeaks(distinct, 64) == 1);
  assert(!processor.ambiguousPeakTies()); // No tolerance-based false ambiguity.
  assert(processor.peakIndex(0) == 20);
  assert(cases >= 10);
  std::map<std::string, std::vector<double>> peakCases;
  std::map<std::string, std::size_t> distances;
  for (const auto& row : readCsv(root + "/pulse_peak_cases.csv")) {
    distances[row[0]] = std::stoul(row[1]);
    assert(peakCases[row[0]].size() == std::stoul(row[2]));
    peakCases[row[0]].push_back(std::stod(row[3]));
  }
  auto cornerExpected = readPeaks(root + "/pulse_peak_expected.csv");
  std::size_t parityPeakCases = 0, unsupportedPeakCases = 0;
  for (const auto& entry : peakCases) {
    const auto& name = entry.first;
    const std::size_t count = processor.detectPeaks(entry.second.data(), entry.second.size(), .3, distances[name]);
    if (name == "mixed_ties_unsupported") {
      assert(processor.ambiguousPeakTies());
      bool same = count == cornerExpected[name].size();
      for (std::size_t i = 0; same && i < count; ++i)
        same = processor.peakIndex(i) == cornerExpected[name][i].sample;
      assert(!same); // This recorded SciPy tie ordering is deliberately not claimed as portable.
      ++unsupportedPeakCases;
      continue;
    }
    assert(count == cornerExpected[name].size());
    for (std::size_t i = 0; i < count; ++i) {
      assert(processor.peakIndex(i) == cornerExpected[name][i].sample);
      near(processor.peakProminence(i), cornerExpected[name][i].prominence, 1e-12, name + " corner prominence");
    }
    ++parityPeakCases;
  }
  assert(!processor.evaluate(nullptr).valid);
  assert(!processor.evaluate(raw.begin()->second.data(), static_cast<Ppg::PulseModel>(99)).valid);
  assert(!processor.evaluate(raw.begin()->second.data(), Ppg::PulseModel::Rule,
                              static_cast<Ppg::PulseOperatingPoint>(99)).valid);
  float invalid[Ppg::PulseLoss::SampleCount] = {};
  invalid[900] = std::numeric_limits<float>::quiet_NaN();
  assert(!processor.evaluate(invalid).valid);
  invalid[900] = std::numeric_limits<float>::infinity();
  assert(!processor.evaluate(invalid).valid);
  assert(unsupportedPeakCases == 1);
  std::cout << "PPG pulse parity: " << cases << " waveforms, " << parityPeakCases
            << " peak cases, all four models / both thresholds passed; workspace="
            << sizeof(Ppg::PulseLoss) << " bytes; " << unsupportedPeakCases
            << " unsupported tie-order case correctly flagged (not a parity pass)\n";
}
