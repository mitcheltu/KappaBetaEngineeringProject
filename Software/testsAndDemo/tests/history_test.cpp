#include "HistoryTransfer.h"
#include <assert.h>
#include <algorithm>
#include <deque>
#include <iostream>
#include <vector>

using namespace History;

static Command command(Opcode opcode, uint16_t token, uint32_t cursor) {
  return Command{static_cast<uint8_t>(opcode), VERSION, token, cursor};
}

static Record sample(uint32_t value) {
  return makeRecord(Kind::PPG, 0x81, uint64_t(value) * 40000, &value, sizeof(value));
}

static void append(Buffer& buffer, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) assert(buffer.append(sample(buffer.next())));
}

static Status status(Transfer& transfer, StatusType expected, uint32_t now, bool sent = true) {
  Status result{};
  assert(transfer.nextStatus(result));
  assert(result.type == static_cast<uint8_t>(expected));
  assert(result.version == VERSION);
  if (sent) transfer.markStatusSent(now);
  return result;
}

static std::vector<uint8_t> fragments(Transfer& transfer, const Status& block,
                                     uint32_t& now, size_t attBytes = 20, uint32_t step = 1) {
  std::vector<uint8_t> result;
  uint8_t frame[260] = {}, repeat[260] = {};
  for (;;) {
    const size_t length = transfer.nextFragment(frame, attBytes);
    if (!length) break;
    assert(length <= std::min(attBytes, MAX_ATT_BYTES));
    // Failed enqueue: asking again must reproduce the same bytes and offset.
    const size_t repeated = transfer.nextFragment(repeat, attBytes);
    assert(repeated == length && memcmp(frame, repeat, length) == 0);
    FragmentHeader header{};
    memcpy(&header, frame, sizeof(header));
    assert(header.token == block.token && header.first == block.first && header.offset == result.size());
    result.insert(result.end(), frame + sizeof(header), frame + length);
    now += step;
    transfer.markFragmentSent(now);
    transfer.markFragmentSent(now); // A duplicate mark cannot consume another frame.
    transfer.tick(now);
  }
  assert(result.size() == size_t(block.count) * sizeof(Record));
  assert(crc32(result.data(), result.size()) == block.detail);
  for (uint32_t i = 0; i < block.count; ++i) {
    Record record{};
    memcpy(&record, result.data() + i * sizeof(Record), sizeof(record));
    assert(record.sequence == block.first + i);
  }
  return result;
}

static void protocolTests() {
  assert(crc32("123456789", 9) == 0xcbf43926u);
  assert(crc32(nullptr, 0) == 0);
  const uint8_t bytes[] = {1, 1, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12};
  Command parsed{};
  assert(parseCommand(bytes, sizeof(bytes), parsed));
  assert(parsed.opcode == 1 && parsed.token == 0x1234 && parsed.cursor == 0x12345678);
  const Command saved = parsed;
  assert(!parseCommand(bytes, 7, parsed) && memcmp(&saved, &parsed, sizeof(parsed)) == 0);
  assert(!parseCommand(nullptr, 8, parsed));
  for (unsigned field = 0; field < 3; ++field) {
    Command invalid = saved;
    if (field == 0) invalid.opcode = 6;
    if (field == 1) invalid.version = 2;
    if (field == 2) invalid.token = 0;
    assert(!parseCommand(&invalid, sizeof(invalid), parsed));
  }
  const uint32_t payload = 0x12345678;
  const auto record = makeRecord(Kind::SURFACE_ZERO, 1, 0x1122334455667788ULL, &payload, sizeof(payload));
  assert(record.kind == 5 && record.flags == 1 && record.reserved == 0);
  for (size_t i = 4; i < 16; ++i) assert(record.payload[i] == 0);
}

static void bufferTests() {
  Buffer empty;
  assert(empty.reset(nullptr, 0));
  assert(!empty.append(sample(1)) && empty.next() == 0 && empty.capacity() == 0);
  assert(!empty.reset(nullptr, 5));
  Record out[20]{};
  assert(empty.copy(0, out, 20) == 0);
  for (uint32_t capacity = 1; capacity <= 17; ++capacity) {
    std::vector<Record> storage(capacity + 2);
    storage.front().sequence = 0xabcd1234;
    storage.back().sequence = 0xdcba4321;
    Buffer buffer;
    assert(buffer.reset(storage.data() + 1, capacity));
    std::deque<uint32_t> retained;
    for (uint32_t i = 0; i < 1200; ++i) {
      Record record = sample(i);
      record.sequence = 123; record.reserved = 123;
      assert(buffer.append(record));
      retained.push_back(i);
      if (retained.size() > capacity) retained.pop_front();
      assert(buffer.next() == i + 1 && buffer.size() == retained.size());
      assert(buffer.oldest() == retained.front());
      const size_t count = buffer.copy(buffer.oldest(), out, 20);
      assert(count == retained.size());
      for (size_t j = 0; j < count; ++j) {
        uint32_t value = 0;
        memcpy(&value, out[j].payload, sizeof(value));
        assert(out[j].sequence == retained[j] && value == retained[j]);
        assert(out[j].reserved == 0 && out[j].timestampUs == uint64_t(value) * 40000);
      }
      if (buffer.oldest()) assert(buffer.copy(buffer.oldest() - 1, out, 20) == 0);
      assert(buffer.copy(buffer.next(), out, 20) == 0);
      assert(buffer.copy(buffer.oldest(), nullptr, 20) == 0);
      assert(buffer.copy(buffer.oldest(), out, 0) == 0);
      assert(buffer.copy(buffer.next() - 1, out, 1) == 1 && out[0].sequence == i);
      assert(storage.front().sequence == 0xabcd1234 && storage.back().sequence == 0xdcba4321);
    }
  }
  Record storage[3]{};
  Buffer terminal;
  assert(terminal.reset(storage, 3, UINT32_MAX - 2));
  append(terminal, 2);
  assert(terminal.next() == UINT32_MAX && terminal.exhausted());
  assert(!terminal.append(sample(0)) && terminal.size() == 2);
  assert(terminal.copy(UINT32_MAX - 2, out, 20) == 2);
  assert(out[1].sequence == UINT32_MAX - 1);
}

static void emptyAndLiveTests() {
  Buffer buffer;
  Transfer transfer(buffer);
  uint32_t now = 0;
  assert(!transfer.handle(command(Opcode::POLL, 1, 0), now));
  assert(status(transfer, StatusType::ERROR, now).first == static_cast<uint32_t>(Error::INVALID_STATE));
  assert(!transfer.handle(command(Opcode::OPEN, 1, 1), now));
  assert(status(transfer, StatusType::ERROR, now).first == static_cast<uint32_t>(Error::BAD_CURSOR));
  assert(transfer.handle(command(Opcode::OPEN, 1, 0), now));
  assert(!transfer.liveEnabled());
  auto ready = status(transfer, StatusType::READY, now, false);
  assert(ready.first == 0 && ready.end == 0 && ready.detail == 0);
  uint8_t frame[20];
  assert(transfer.nextFragment(frame, sizeof(frame)) == 0); // READY must precede data.
  assert(!transfer.handle(command(Opcode::LIVE, 1, 0), now));
  transfer.markStatusSent(now);
  assert(!transfer.handle(command(Opcode::LIVE, 1, 0), now)); // END not sent yet.
  status(transfer, StatusType::END, now);
  assert(!transfer.handle(command(Opcode::LIVE, 2, 0), now));
  assert(!transfer.handle(command(Opcode::LIVE, 1, 1), now));
  assert(transfer.handle(command(Opcode::LIVE, 1, 0), now));
  assert(transfer.liveEnabled());
  auto live = status(transfer, StatusType::LIVE, now);
  assert(live.first == 0 && live.end == 0 && live.detail == 0 && !transfer.active());
  // Five unacknowledgeable LIVE retries end quietly, without revoking live mode.
  for (unsigned i = 0; i < Transfer::MAX_RETRIES; ++i) {
    now += Transfer::RETRY_MS;
    transfer.tick(now);
    status(transfer, StatusType::LIVE, now);
  }
  now += Transfer::RETRY_MS;
  transfer.tick(now);
  Status none{};
  assert(!transfer.nextStatus(none) && transfer.liveEnabled());
  assert(transfer.handle(command(Opcode::LIVE, 1, 0), now));
  status(transfer, StatusType::LIVE, now); // Explicit resend still repairs a lost status.
  now += Transfer::RETRY_MS;
  transfer.tick(now); // Retry enqueue can be pending although the client got the first LIVE.
  status(transfer, StatusType::LIVE, now, false);
  assert(transfer.handle(command(Opcode::POLL, 2, 0), now));
  assert(transfer.liveEnabled());
  assert(!transfer.handle(command(Opcode::ABORT, 1, 0), now));
  assert(transfer.handle(command(Opcode::ABORT, 2, 0), now));
  assert(!transfer.active() && transfer.liveEnabled());
  transfer.disconnect();
  assert(!transfer.liveEnabled() && !transfer.active());
  assert(!transfer.handle(command(Opcode::LIVE, 2, 0), now));
}

static void snapshotAndPollingTests() {
  Record storage[128]{};
  Buffer buffer;
  buffer.reset(storage, 128);
  append(buffer, 70);
  Transfer transfer(buffer);
  transfer.setFlags(ALLOCATED | ML_RESERVED);
  uint32_t now = 10;
  assert(transfer.handle(command(Opcode::OPEN, 42, 0), now));
  append(buffer, 20);
  auto ready = status(transfer, StatusType::READY, now);
  assert(ready.end == 70 && ready.detail == 128 && ready.flags == (ALLOCATED | ML_RESERVED));
  auto block = status(transfer, StatusType::BLOCK, now);
  assert(block.first == 0 && block.end == 70 && block.count == 64);
  assert(!transfer.handle(command(Opcode::ACK, 42, 64), now)); // First send incomplete.
  uint8_t scratch[260]{};
  assert(transfer.nextFragment(scratch, 8) == 0);
  assert(transfer.nextFragment(nullptr, 244) == 0);
  auto first = fragments(transfer, block, now, 20, 100);
  assert(first.size() == 2048); // 171 frames take >17s, but each makes progress.
  assert(!transfer.handle(command(Opcode::ACK, 41, 64), now));
  assert(!transfer.handle(command(Opcode::ACK, 42, 70), now)); // BLOCK.end is not its ACK cursor.
  Status none{};
  transfer.tick(now + 1999);
  assert(!transfer.nextStatus(none));
  transfer.tick(now + 2000);
  status(transfer, StatusType::BLOCK, now + 2000, false);
  // ACK from the completed first send is accepted before the retry is sent.
  assert(transfer.handle(command(Opcode::ACK, 42, 64), now + 2001));
  assert(!transfer.handle(command(Opcode::ACK, 42, 64), now + 2001));
  now += 2001;
  block = status(transfer, StatusType::BLOCK, now);
  assert(block.first == 64 && block.end == 70 && block.count == 6);
  fragments(transfer, block, now, 1000); // Negotiated payload is still clamped to 244.
  assert(transfer.handle(command(Opcode::ACK, 42, 70), now));
  auto end = status(transfer, StatusType::END, now);
  assert(end.first == 70 && end.end == 70);
  assert(transfer.handle(command(Opcode::LIVE, 42, 70), now));
  auto live = status(transfer, StatusType::LIVE, now);
  assert(live.first == 70 && live.end == 90);
  assert(transfer.handle(command(Opcode::POLL, 43, 70), now));
  status(transfer, StatusType::READY, now);
  append(buffer, 4);
  block = status(transfer, StatusType::BLOCK, now);
  assert(block.first == 70 && block.end == 90 && block.count == 20);
  fragments(transfer, block, now, 244);
  assert(transfer.handle(command(Opcode::ACK, 43, 90), now));
  status(transfer, StatusType::END, now);
  assert(transfer.handle(command(Opcode::LIVE, 43, 90), now));
  status(transfer, StatusType::LIVE, now);
  assert(transfer.liveEnabled() && buffer.next() == 94); // New tail waits for the next POLL.
}

static void overwriteAndGapTests() {
  Record storage[130]{};
  Buffer buffer;
  buffer.reset(storage, 130);
  append(buffer, 130);
  Transfer transfer(buffer);
  uint32_t now = 100;
  transfer.handle(command(Opcode::OPEN, 7, 0), now);
  status(transfer, StatusType::READY, now);
  auto block = status(transfer, StatusType::BLOCK, now);
  auto bytes = fragments(transfer, block, now, 244);
  append(buffer, 200); // All staged and remaining snapshot records are overwritten.
  transfer.tick(now + 2000);
  auto retry = status(transfer, StatusType::BLOCK, now + 2000);
  assert(memcmp(&block, &retry, sizeof(block)) == 0);
  now += 2000;
  auto repeated = fragments(transfer, retry, now, 244);
  assert(bytes == repeated); // Retries never reread the overwritten ring.
  assert(transfer.handle(command(Opcode::ACK, 7, 64), now));
  auto gap = status(transfer, StatusType::GAP, now, false);
  assert(gap.first == 64 && gap.end == 130); // Cap loss at fixed snapshot boundary.
  assert(!transfer.handle(command(Opcode::ACK, 7, 130), now));
  transfer.markStatusSent(now);
  assert(!transfer.handle(command(Opcode::ACK, 7, 200), now));
  transfer.tick(now + 2000);
  assert(transfer.handle(command(Opcode::ACK, 7, 130), now + 2000));
  status(transfer, StatusType::END, now + 2000);

  // An initially stale browser cursor reports the exact retained range first.
  transfer.handle(command(Opcode::OPEN, 8, 0), now);
  auto ready = status(transfer, StatusType::READY, now);
  assert(ready.first == 200 && ready.end == 330);
  gap = status(transfer, StatusType::GAP, now);
  assert(gap.first == 0 && gap.end == 200);
  assert(transfer.handle(command(Opcode::ACK, 8, 200), now));
  block = status(transfer, StatusType::BLOCK, now, false);
  assert(block.first == 200 && block.count == 64 && block.end == 330);
  // A rejected OPEN reports its own error without discarding valid staging.
  assert(!transfer.handle(command(Opcode::OPEN, 9, 999), now));
  auto error = status(transfer, StatusType::ERROR, now);
  assert(error.token == 9 && error.first == static_cast<uint32_t>(Error::BAD_CURSOR));
  auto preserved = status(transfer, StatusType::BLOCK, now);
  assert(memcmp(&block, &preserved, sizeof(block)) == 0);
  assert(transfer.handle(command(Opcode::ABORT, 8, 0), now));
  assert(!transfer.active() && !transfer.liveEnabled());
}

static void timeoutAndTerminalTests() {
  Record storage[2]{};
  Buffer buffer;
  buffer.reset(storage, 2, UINT32_MAX - 2);
  append(buffer, 2);
  Transfer transfer(buffer);
  transfer.setFlags(ALLOCATED | SEQUENCE_EXHAUSTED);
  uint32_t now = UINT32_MAX - 1000;
  transfer.handle(command(Opcode::OPEN, 3, UINT32_MAX - 2), now);
  status(transfer, StatusType::READY, now);
  auto block = status(transfer, StatusType::BLOCK, now);
  assert(block.end == UINT32_MAX && block.count == 2);
  fragments(transfer, block, now);
  assert(transfer.handle(command(Opcode::ACK, 3, UINT32_MAX), now));
  status(transfer, StatusType::END, now);
  for (unsigned i = 0; i < Transfer::MAX_RETRIES; ++i) {
    now += Transfer::RETRY_MS; // Crosses the millis() boundary.
    transfer.tick(now);
    auto end = status(transfer, StatusType::END, now);
    assert(end.first == UINT32_MAX && end.end == UINT32_MAX);
  }
  now += Transfer::RETRY_MS;
  transfer.tick(now);
  auto error = status(transfer, StatusType::ERROR, now);
  assert(error.first == static_cast<uint32_t>(Error::TIMEOUT));
  assert(!transfer.liveEnabled() && !transfer.active() && buffer.size() == 2);

  // A notification queue that never accepts READY also has a bounded timeout.
  transfer.handle(command(Opcode::OPEN, 4, UINT32_MAX), now);
  for (unsigned i = 0; i < Transfer::MAX_RETRIES; ++i) {
    now += Transfer::RETRY_MS;
    transfer.tick(now);
    status(transfer, StatusType::READY, now, false);
  }
  now += Transfer::RETRY_MS;
  transfer.tick(now);
  status(transfer, StatusType::ERROR, now);
}

int main() {
  protocolTests(); bufferTests(); emptyAndLiveTests(); snapshotAndPollingTests();
  overwriteAndGapTests(); timeoutAndTerminalTests();
  std::cout << "History protocol, ring bounds/overwrite, snapshots, immutable retries, ACK/gap/live recovery and timeout tests passed\n";
}
