// Golden parity with the unchanged notebook 03 Python replay plus device lifecycle tests.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "OxygenConcern.h"

namespace {

std::vector<std::string> fields(const std::string& row) {
  std::vector<std::string> result;
  std::istringstream stream(row);
  for (std::string field; std::getline(stream, field, ',');) result.push_back(field);
  return result;
}

void golden(const char* path) {
  std::ifstream input(path);
  assert(input.good());
  std::string row, name;
  std::getline(input, row);  // CSV header.
  Ppg::OxygenConcern rule;
  size_t cases = 0, rows = 0;
  while (std::getline(input, row)) {
    const auto f = fields(row);
    assert(f.size() == 9);
    if (f[0] != name) {
      Ppg::OxygenConfig config;
      config.start = std::stof(f[1]);
      config.full = std::stof(f[2]);
      config.autoClear = std::stoi(f[3]) != 0;
      rule = Ppg::OxygenConcern(config);
      name = f[0];
      ++cases;
    }
    const uint32_t second = static_cast<uint32_t>(std::stoull(f[4]));
    const bool ready = rule.update(std::stof(f[5]), std::stoi(f[6]) != 0, second);
    const float expected = std::stof(f[7]);
    const bool expectedReady = std::isfinite(expected);
    const bool expectedActive = std::stoi(f[8]) != 0;
    if (ready != expectedReady || rule.ready() != expectedReady ||
        rule.active() != expectedActive ||
        (expectedReady && std::fabs(rule.concern() - expected) > 2e-6f) ||
        (!expectedReady && !std::isnan(rule.concern()))) {
      std::fprintf(stderr, "Mismatch case=%s second=%u concern=%g expected=%g active=%d expected=%d\n",
                   name.c_str(), second, static_cast<double>(rule.concern()),
                   static_cast<double>(expected), rule.active(), expectedActive);
      assert(false);
    }
    ++rows;
  }
  assert(cases >= 19 && rows >= 600);
  std::printf("Oxygen Python golden parity passed: %zu rows, %zu cases\n", rows, cases);
}

void lifecycle() {
  Ppg::OxygenConcern rule;
  assert(!rule.ready() && !rule.active());
  for (uint32_t t = 0; t <= 5; ++t) rule.update(60, true, t);
  assert(rule.active() && rule.concern() == 1.0f);
  rule.acknowledge();
  assert(!rule.active() && rule.concern() == 1.0f);
  for (uint32_t t = 6; t < 9; ++t) {
    assert(rule.update(60, true, t));
    assert(!rule.active());
  }
  rule.update(60, true, 9);
  assert(rule.active());

  // A missing reading cannot clear a latched alert or reuse the stale estimate.
  assert(!rule.update(60, false, 10));
  assert(rule.active() && !rule.ready() && std::isnan(rule.concern()));
  assert(!rule.update(98, true, 11));
  assert(!rule.update(98, true, 12));
  assert(rule.update(98, true, 13));
  assert(rule.active() && rule.concern() > 0.7f);  // EMA was retained across gap.

  rule.reset();
  assert(!rule.active() && !rule.ready());
  assert(!rule.update(98, true, 100));
  assert(!rule.update(98, true, 101));
  assert(rule.update(98, true, 102));
  assert(rule.concern() == 0.0f);
  assert(!rule.update(std::numeric_limits<float>::infinity(), true, 103));

  // Invalid configuration yields no score, avoiding division by zero/NaN.
  Ppg::OxygenConfig bad;
  bad.full = bad.start;
  Ppg::OxygenConcern invalid(bad);
  for (uint32_t t = 0; t < 10; ++t) assert(!invalid.update(60, true, t));
  assert(!invalid.active());

  // Confirmation and recovery durations are measured elapsed seconds, not
  // number of samples. Zero-second confirmation is useful for boundary checks.
  Ppg::OxygenConfig immediate;
  immediate.confirmSeconds = 0;
  Ppg::OxygenConcern zeroDelay(immediate);
  zeroDelay.update(70, true, 0);
  zeroDelay.update(70, true, 1);
  zeroDelay.update(70, true, 2);
  assert(zeroDelay.active());
}

}  // namespace

int main(int argc, char** argv) {
  golden(argc > 1 ? argv[1] : "Software/tests/ppg/oxygen_golden.csv");
  lifecycle();
  std::puts("Oxygen concern lifecycle tests passed");
}
