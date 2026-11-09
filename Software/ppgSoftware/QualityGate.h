#pragma once

#include <cstdint>
#include <limits>

namespace Ppg {

// A session qualification gate, NOT the learned HR-agreement model from
// 02_hr_reliability.ipynb. That model requires red/IR AND motion features and
// predicts HR accuracy, not whether this wearer's chest pulse is observable.
// No verified PPG-only pulse-visibility model is supplied by that notebook.
//
// An operator/adapter may qualify a wearer only after independent reference
// observation at the present sensor placement. The caller supplies the policy
// lifetime explicitly; no physiological validity duration is inferred here.
// Revoke on wearer/placement changes, contact loss or FIFO discontinuities.
// Qualification never depends on pulse count in the window being diagnosed:
// otherwise an actual pulse-loss event would disable its own detection.
// This single-owner type performs no allocation and is not thread-safe.
class QualityGate {
 public:
  enum class Reason : std::uint8_t {
    ACCEPTED,
    WINDOW_INVALID,
    NOT_QUALIFIED,
    WEARER_MISMATCH,
    TIME_BEFORE_QUALIFICATION,
    QUALIFICATION_EXPIRED
  };

  struct Decision {
    bool accepted;
    Reason reason;
  };

  // referenceVerified is an explicit assertion supplied by the observer,
  // never a property inferred from a present, missing or irregular pulse.
  // An invalid qualification request revokes any previous qualification.
  bool qualify(std::uint32_t wearerId, std::uint64_t verifiedAtUs,
               std::uint64_t validForUs, bool referenceVerified) {
    revoke();
    if (!referenceVerified || wearerId == 0 || validForUs == 0 ||
        validForUs > std::numeric_limits<std::uint64_t>::max() - verifiedAtUs)
      return false;
    wearerId_ = wearerId;
    verifiedAtUs_ = verifiedAtUs;
    expiresAtUs_ = verifiedAtUs + validForUs;
    qualified_ = true;
    return true;
  }

  void revoke() {
    qualified_ = false;
    wearerId_ = 0;
    verifiedAtUs_ = expiresAtUs_ = 0;
  }

  Decision evaluate(bool continuousWindow, std::uint64_t nowUs,
                    std::uint32_t wearerId) const {
    if (!continuousWindow) return {false, Reason::WINDOW_INVALID};
    if (!qualified_) return {false, Reason::NOT_QUALIFIED};
    if (wearerId != wearerId_) return {false, Reason::WEARER_MISMATCH};
    if (nowUs < verifiedAtUs_) return {false, Reason::TIME_BEFORE_QUALIFICATION};
    if (nowUs >= expiresAtUs_) return {false, Reason::QUALIFICATION_EXPIRED};
    return {true, Reason::ACCEPTED};
  }

 private:
  bool qualified_ = false;
  std::uint32_t wearerId_ = 0;
  std::uint64_t verifiedAtUs_ = 0;
  std::uint64_t expiresAtUs_ = 0;
};

}  // namespace Ppg
