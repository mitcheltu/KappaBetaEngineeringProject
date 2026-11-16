#include "PpgWindow.h"
#include "QualityGate.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {
using Ppg::PpgWindow;
using Ppg::QualityGate;

void ringAndGaps() {
  PpgWindow ring;
  std::array<float, PpgWindow::kSamples> copy;
  copy.fill(-7.0f);
  assert(!ring.copy(copy.data()));
  assert(copy.front() == -7.0f);
  assert(!ring.copy(nullptr));
  assert(ring.count() == 0 && !ring.full());

  // Exact chronology across more than two wraps, including time zero.
  for (std::size_t i = 0; i < PpgWindow::kSamples * 3 + 17; ++i) {
    assert(ring.add(static_cast<float>(i), i * 10000, true));
    assert(ring.count() == (i < PpgWindow::kSamples ? i + 1 : PpgWindow::kSamples));
  }
  assert(ring.full());
  assert(ring.copy(copy.data()));
  for (std::size_t i = 0; i < copy.size(); ++i)
    assert(copy[i] == static_cast<float>(PpgWindow::kSamples * 2 + 17 + i));

  auto gen = ring.generation();
  const auto last = ring.lastSampleTimeUs();
  assert(ring.add(123.0f, last + 20000, true));  // Lost one sample.
  assert(ring.count() == 1 && ring.generation() == gen + 1);
  assert(!ring.copy(copy.data()));
  gen = ring.generation();
  assert(ring.add(124.0f, last + 20000, true));  // Duplicate timestamp.
  assert(ring.count() == 1 && ring.generation() == gen + 1);
  gen = ring.generation();
  assert(ring.add(125.0f, 0, true));  // Timestamp/clock reset.
  assert(ring.count() == 1 && ring.generation() == gen + 1);
  assert(!ring.add(126.0f, 10000, false));
  assert(ring.count() == 0);
  assert(!ring.add(std::numeric_limits<float>::quiet_NaN(), 20000, true));
  assert(!ring.add(std::numeric_limits<float>::infinity(), 30000, true));
  assert(ring.count() == 0);

  // Tolerance boundaries are inclusive; timestamp arithmetic doesn't wrap.
  assert(ring.add(1.0f, 0, true));
  assert(ring.add(2.0f, 8000, true));
  assert(ring.add(3.0f, 20000, true));
  assert(ring.count() == 3);
  assert(ring.add(4.0f, 27999, true));
  assert(ring.count() == 1);
  ring.reset();
  assert(ring.add(1.0f, std::numeric_limits<std::uint64_t>::max() - 1, true));
  assert(ring.add(2.0f, 9998, true));
  assert(ring.count() == 1);

  PpgWindow zeroPeriod(0, 0), invalidTolerance(10000, 10000);
  assert(!zeroPeriod.add(1.0f, 0, true));
  assert(!invalidTolerance.add(1.0f, 0, true));
}

void gateLifetimeAndEvidence() {
  QualityGate gate;
  using Reason = QualityGate::Reason;
  assert(gate.evaluate(true, 0, 1).reason == Reason::NOT_QUALIFIED);
  assert(!gate.qualify(1, 100, 1000, false));
  assert(!gate.qualify(0, 100, 1000, true));
  assert(!gate.qualify(1, 100, 0, true));
  assert(!gate.qualify(1, std::numeric_limits<std::uint64_t>::max() - 10, 11, true));
  assert(gate.qualify(9, 100, 1000, true));
  assert(gate.evaluate(true, 99, 9).reason == Reason::TIME_BEFORE_QUALIFICATION);
  assert(gate.evaluate(true, 100, 9).accepted);
  assert(gate.evaluate(true, 1099, 9).accepted);
  assert(gate.evaluate(true, 1100, 9).reason == Reason::QUALIFICATION_EXPIRED);
  assert(gate.evaluate(true, 100, 10).reason == Reason::WEARER_MISMATCH);
  assert(gate.evaluate(false, 100, 9).reason == Reason::WINDOW_INVALID);
  gate.revoke();
  assert(gate.evaluate(true, 100, 9).reason == Reason::NOT_QUALIFIED);
  assert(gate.qualify(9, 100, 1000, true));
  assert(!gate.qualify(9, 100, 1000, false));
  assert(!gate.evaluate(true, 100, 9).accepted);
}

void missingPulseDoesNotDisqualify() {
  PpgWindow ring;
  QualityGate gate;
  std::array<float, PpgWindow::kSamples> copy;
  // No amplitude/beat/HR checks belong in this gate: a full finite flat
  // transport-valid window must still reach the pulse-loss detector.
  assert(gate.qualify(1, 0, 60000000, true));
  for (std::size_t i = 0; i < PpgWindow::kSamples; ++i)
    assert(ring.add(90000.0f, i * 10000, true));
  assert(ring.copy(copy.data()));
  assert(gate.evaluate(ring.full(), ring.lastSampleTimeUs(), 1).accepted);
  const auto gen = ring.generation();
  ring.add(90000.0f, ring.lastSampleTimeUs() + 30000, true);
  if (ring.generation() != gen) gate.revoke();
  assert(!gate.evaluate(ring.full(), ring.lastSampleTimeUs(), 1).accepted);
  assert(gate.evaluate(true, ring.lastSampleTimeUs(), 1).reason ==
         QualityGate::Reason::NOT_QUALIFIED);
}
}  // namespace

int main() {
  ringAndGaps();
  gateLifetimeAndEvidence();
  missingPulseDoesNotDisqualify();
  std::cout << "PPG window and quality gate tests passed\n";
}
