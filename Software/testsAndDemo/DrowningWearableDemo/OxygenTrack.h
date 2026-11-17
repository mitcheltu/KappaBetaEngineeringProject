#pragma once
#include <stdint.h>
#include "OxygenConcern.h"

// Small seam so a future oxygen variant (e.g. threshold_persist or a
// pre-registered trend/projection rule from notebook 03 v5) can replace the
// ported concern_level rule without touching the scenario controller.
namespace Scenario {
class OxygenTrack {
 public:
  virtual ~OxygenTrack() = default;
  virtual void reset() = 0;
  // One reading per second. Returns true when a concern value was produced.
  virtual bool update(float spo2, bool valid, uint32_t second) = 0;
  virtual float concern() const = 0;  // NaN while unavailable.
  virtual bool active() const = 0;    // Latched alert.
};

// Notebook 03 absolute concern_level, deep_breath_hold anchors (80 -> 60 %),
// latched (autoClear=false). Research rule, deployable=false in the notebook.
class ConcernLevelTrack final : public OxygenTrack {
 public:
  explicit ConcernLevelTrack(const Ppg::OxygenConfig& config = Ppg::OxygenConfig())
      : rule_(config) {}
  void reset() override { rule_.reset(); }
  bool update(float spo2, bool valid, uint32_t second) override {
    return rule_.update(spo2, valid, second);
  }
  float concern() const override { return rule_.concern(); }
  bool active() const override { return rule_.active(); }

 private:
  Ppg::OxygenConcern rule_;
};
}  // namespace Scenario
