#pragma once
#include "HistoryBuffer.h"

namespace History {
// One loop owns this object and Buffer. BLE callbacks must queue commands.
// A successful notify enqueue is reported with mark*Sent; a failed enqueue
// leaves the same status/fragment available. No dynamic allocation or BLE calls.
class Transfer {
public:
  static constexpr uint32_t RETRY_MS = 2000;
  static constexpr uint8_t MAX_RETRIES = 5;

  explicit Transfer(Buffer& buffer) : buffer_(buffer) {}
  void setFlags(uint16_t flags) { flags_ = flags; }
  bool liveEnabled() const { return liveEnabled_; }
  bool active() const { return phase_ != Phase::IDLE; }
  uint16_t token() const { return token_; }
  uint32_t cursor() const { return cursor_; }
  uint32_t snapshotEnd() const { return snapshotEnd_; }
  size_t stagedCount() const { return blockCount_; }

  void disconnect() {
    phase_ = Phase::IDLE;
    token_ = 0;
    cursor_ = snapshotEnd_ = 0;
    liveEnabled_ = liveReceiptValid_ = idleLiveRetry_ = false;
    errorPending_ = false;
    blockCount_ = fragmentOffset_ = 0;
    ackEligible_ = endEligible_ = false;
    retries_ = 0;
    invalidateProposal();
  }

  // True means accepted. False means ignored or rejected (possibly with ERROR).
  bool handle(const Command& command, uint32_t nowMs) {
    if (!validCommand(command)) return false;
    const auto opcode = static_cast<Opcode>(command.opcode);
    if (opcode == Opcode::OPEN || opcode == Opcode::POLL) {
      if (command.cursor > buffer_.next()) {
        reportError(command.token, Error::BAD_CURSOR, buffer_.next());
        return false;
      }
      const bool canPoll = phase_ == Phase::IDLE || (phase_ == Phase::LIVE && liveReceiptValid_);
      if (opcode == Opcode::POLL && (!liveEnabled_ || !canPoll)) {
        reportError(command.token, Error::INVALID_STATE, snapshotEnd_);
        return false;
      }
      // OPEN is an explicit restart. POLL is allowed only after LIVE.
      token_ = command.token;
      cursor_ = command.cursor;
      snapshotEnd_ = buffer_.next();
      if (opcode == Opcode::OPEN) liveEnabled_ = false;
      liveReceiptValid_ = idleLiveRetry_ = errorPending_ = false;
      ackEligible_ = endEligible_ = false;
      blockCount_ = fragmentOffset_ = 0;
      retries_ = 0;
      lastProgressMs_ = nowMs;
      status_ = makeStatus(StatusType::READY, buffer_.oldest(), snapshotEnd_, buffer_.capacity());
      phase_ = Phase::READY;
      invalidateProposal();
      return true;
    }
    if (command.token != token_) return false;
    if (opcode == Opcode::ABORT) {
      phase_ = Phase::IDLE;
      liveReceiptValid_ = idleLiveRetry_ = errorPending_ = false;
      ackEligible_ = endEligible_ = false;
      blockCount_ = fragmentOffset_ = 0;
      invalidateProposal();
      return true;
    }
    if (opcode == Opcode::ACK) {
      // A delayed ACK is valid during a retry once the first full send completed.
      const bool block = phase_ == Phase::BLOCK || phase_ == Phase::FRAGMENTS || phase_ == Phase::WAIT_ACK;
      const bool gap = phase_ == Phase::GAP || phase_ == Phase::WAIT_GAP_ACK;
      const uint32_t expected = block ? blockFirst_ + blockCount_ : status_.end;
      if (!ackEligible_ || (!block && !gap) || command.cursor != expected) return false;
      cursor_ = expected;
      prepareNext(nowMs);
      return true;
    }
    if (opcode == Opcode::LIVE) {
      if (endEligible_ && (phase_ == Phase::END || phase_ == Phase::WAIT_LIVE) &&
          command.cursor == snapshotEnd_) {
        liveEnabled_ = true;
        endEligible_ = false;
        liveReceiptValid_ = true;
        cachedLive_ = makeStatus(StatusType::LIVE, command.cursor, buffer_.next(), buffer_.capacity());
      } else if (!liveReceiptValid_ || command.cursor != cachedLive_.first ||
                 (phase_ != Phase::IDLE && phase_ != Phase::LIVE)) {
        return false;
      }
      // Repeated LIVE repairs a lost LIVE notification without reopening data.
      status_ = cachedLive_;
      phase_ = Phase::LIVE;
      idleLiveRetry_ = false;
      retries_ = 0;
      lastProgressMs_ = nowMs;
      invalidateProposal();
      return true;
    }
    return false;
  }

  void tick(uint32_t nowMs) {
    if (phase_ == Phase::IDLE) {
      if (!idleLiveRetry_ || uint32_t(nowMs - lastProgressMs_) < RETRY_MS) return;
      if (retries_ >= MAX_RETRIES) { idleLiveRetry_ = false; return; }
      ++retries_;
      status_ = cachedLive_;
      phase_ = Phase::LIVE;
      lastProgressMs_ = nowMs;
      invalidateProposal();
      return;
    }
    if (uint32_t(nowMs - lastProgressMs_) < RETRY_MS) return;
    if (retries_ >= MAX_RETRIES) {
      const bool wasLive = phase_ == Phase::LIVE;
      phase_ = Phase::IDLE;
      idleLiveRetry_ = false;
      ackEligible_ = endEligible_ = false;
      blockCount_ = fragmentOffset_ = 0;
      invalidateProposal();
      // LIVE has no ACK. Silence after its retry budget is not a failure.
      if (!wasLive) reportError(token_, Error::TIMEOUT, snapshotEnd_);
      return;
    }
    ++retries_;
    lastProgressMs_ = nowMs;
    switch (phase_) {
      case Phase::BLOCK: case Phase::FRAGMENTS: case Phase::WAIT_ACK:
        phase_ = Phase::BLOCK;
        fragmentOffset_ = 0;
        break;
      case Phase::GAP: case Phase::WAIT_GAP_ACK: phase_ = Phase::GAP; break;
      case Phase::END: case Phase::WAIT_LIVE: phase_ = Phase::END; break;
      default: break; // READY and LIVE retry their unchanged status.
    }
    invalidateProposal();
  }

  bool nextStatus(Status& out) {
    proposedStatus_ = StatusSource::NONE;
    if (errorPending_) {
      out = errorStatus_;
      proposedStatus_ = StatusSource::ERROR;
    } else if (phase_ == Phase::READY || phase_ == Phase::BLOCK || phase_ == Phase::GAP ||
               phase_ == Phase::END || phase_ == Phase::LIVE) {
      out = status_;
      proposedStatus_ = StatusSource::MAIN;
    } else return false;
    out.flags = flags_;
    return true;
  }

  void markStatusSent(uint32_t nowMs) {
    const auto source = proposedStatus_;
    proposedStatus_ = StatusSource::NONE;
    if (source == StatusSource::ERROR) { errorPending_ = false; return; }
    if (source != StatusSource::MAIN) return;
    lastProgressMs_ = nowMs;
    switch (phase_) {
      case Phase::READY: prepareNext(nowMs); break;
      case Phase::BLOCK: phase_ = Phase::FRAGMENTS; fragmentOffset_ = 0; break;
      case Phase::GAP: phase_ = Phase::WAIT_GAP_ACK; ackEligible_ = true; break;
      case Phase::END: phase_ = Phase::WAIT_LIVE; endEligible_ = true; break;
      case Phase::LIVE: phase_ = Phase::IDLE; idleLiveRetry_ = true; break;
      default: break;
    }
  }

  // Returns the complete frame length including its 8-byte header, or zero.
  size_t nextFragment(void* output, size_t maxAttBytes) {
    proposedFragmentBytes_ = 0;
    if (phase_ != Phase::FRAGMENTS || !output || maxAttBytes <= sizeof(FragmentHeader)) return 0;
    if (maxAttBytes > MAX_ATT_BYTES) maxAttBytes = MAX_ATT_BYTES;
    const size_t remaining = size_t(blockCount_) * sizeof(Record) - fragmentOffset_;
    const size_t room = maxAttBytes - sizeof(FragmentHeader);
    const size_t bytes = remaining < room ? remaining : room;
    if (bytes == 0) return 0;
    const FragmentHeader header{token_, fragmentOffset_, blockFirst_};
    memcpy(output, &header, sizeof(header));
    memcpy(static_cast<uint8_t*>(output) + sizeof(header),
           reinterpret_cast<const uint8_t*>(staged_) + fragmentOffset_, bytes);
    proposedFragmentBytes_ = static_cast<uint16_t>(bytes);
    proposedFragmentOffset_ = fragmentOffset_;
    return sizeof(header) + bytes;
  }

  void markFragmentSent(uint32_t nowMs) {
    if (phase_ != Phase::FRAGMENTS || proposedFragmentBytes_ == 0 ||
        proposedFragmentOffset_ != fragmentOffset_) return;
    fragmentOffset_ += proposedFragmentBytes_;
    proposedFragmentBytes_ = 0;
    lastProgressMs_ = nowMs;
    if (fragmentOffset_ == size_t(blockCount_) * sizeof(Record)) {
      phase_ = Phase::WAIT_ACK;
      ackEligible_ = true;
    }
  }

private:
  enum class Phase { IDLE, READY, BLOCK, FRAGMENTS, WAIT_ACK, GAP, WAIT_GAP_ACK, END, WAIT_LIVE, LIVE };
  enum class StatusSource { NONE, MAIN, ERROR };

  Status makeStatus(StatusType type, uint32_t first, uint32_t end, uint32_t detail = 0,
                    uint16_t count = 0) const {
    return Status{static_cast<uint8_t>(type), VERSION, token_, first, end, detail, count, flags_};
  }

  void reportError(uint16_t token, Error error, uint32_t end) {
    errorStatus_ = Status{static_cast<uint8_t>(StatusType::ERROR), VERSION, token,
                          static_cast<uint32_t>(error), end, 0, 0, flags_};
    errorPending_ = true;
    proposedStatus_ = StatusSource::NONE;
  }

  void invalidateProposal() {
    proposedStatus_ = StatusSource::NONE;
    proposedFragmentBytes_ = 0;
  }

  void prepareNext(uint32_t nowMs) {
    retries_ = 0;
    lastProgressMs_ = nowMs;
    ackEligible_ = endEligible_ = false;
    blockCount_ = fragmentOffset_ = 0;
    invalidateProposal();
    if (cursor_ >= snapshotEnd_) {
      status_ = makeStatus(StatusType::END, snapshotEnd_, snapshotEnd_);
      phase_ = Phase::END;
    } else if (cursor_ < buffer_.oldest()) {
      const uint32_t end = buffer_.oldest() < snapshotEnd_ ? buffer_.oldest() : snapshotEnd_;
      status_ = makeStatus(StatusType::GAP, cursor_, end);
      phase_ = Phase::GAP;
    } else {
      size_t count = snapshotEnd_ - cursor_;
      if (count > MAX_BLOCK_RECORDS) count = MAX_BLOCK_RECORDS;
      blockCount_ = static_cast<uint16_t>(buffer_.copy(cursor_, staged_, count));
      if (blockCount_ == 0) {
        phase_ = Phase::IDLE;
        reportError(token_, Error::INVALID_STATE, snapshotEnd_);
        return;
      }
      blockFirst_ = cursor_;
      status_ = makeStatus(StatusType::BLOCK, cursor_, snapshotEnd_,
                           crc32(staged_, size_t(blockCount_) * sizeof(Record)), blockCount_);
      phase_ = Phase::BLOCK;
    }
  }

  Buffer& buffer_;
  Record staged_[MAX_BLOCK_RECORDS] = {};
  Status status_{}, cachedLive_{}, errorStatus_{};
  Phase phase_ = Phase::IDLE;
  StatusSource proposedStatus_ = StatusSource::NONE;
  uint32_t cursor_ = 0, snapshotEnd_ = 0, blockFirst_ = 0, lastProgressMs_ = 0;
  uint16_t token_ = 0, flags_ = 0, blockCount_ = 0, fragmentOffset_ = 0;
  uint16_t proposedFragmentBytes_ = 0, proposedFragmentOffset_ = 0;
  uint8_t retries_ = 0;
  bool ackEligible_ = false, endEligible_ = false, liveEnabled_ = false;
  bool liveReceiptValid_ = false, idleLiveRetry_ = false, errorPending_ = false;
};
} // namespace History
