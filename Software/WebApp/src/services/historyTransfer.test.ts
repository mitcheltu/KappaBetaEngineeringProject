import assert from 'node:assert/strict';
import test from 'node:test';
import { createHistoryTransfer, type HistoryProgress } from './historyTransfer';
import { MemoryHistoryStore, blockStatus, command, flush, fragment, identity, records, status } from './historyTestFixtures';

function fixture(store = new MemoryHistoryStore(), flags = 3) {
  const writes: ReturnType<typeof command>[] = [], errors: Error[] = [], progress: HistoryProgress[] = [];
  let time = 0, current: DataView = status(1, 0), writer: ((bytes: Uint8Array) => Promise<void>) | undefined;
  const transfer = createHistoryTransfer({ identity: { ...identity, flags }, store, initialToken: 0, now: () => time,
    write: async bytes => { writes.push(command(bytes)); await writer?.(bytes); }, readStatus: async () => current,
    onProgress: p => progress.push(p), onError: e => errors.push(e), pollIntervalMs: 5 });
  const start = () => { const promise = transfer.start(); void promise.catch(() => {}); return promise; };
  return { transfer, writes, errors, progress, store, start, time: (value: number) => { time = value; },
    setStatus: (value: DataView) => { current = value; }, setWriter: (value: (bytes: Uint8Array) => Promise<void>) => { writer = value; } };
}
async function finish(f: ReturnType<typeof fixture>, cursor: number, token = 1) {
  f.transfer.handleStatus(status(3, token, cursor)); await flush();
  f.transfer.handleStatus(status(5, token, cursor, cursor + 2, 131072)); await flush();
}

test('no ACK before durable commit; retry races cannot block the following block', async t => {
  const f = fixture(); t.after(() => f.transfer.cancel());
  const original = f.store.commitBlock.bind(f.store); let complete!: () => void;
  f.store.commitBlock = async (...args) => { await new Promise<void>(resolve => { complete = resolve; }); return original(...args); };
  const initial = f.start(); await flush();
  f.transfer.handleStatus(status(1, 0)); // idle token from a read racing OPEN
  f.transfer.handleStatus(status(2, 99, 0, 4)); // stale token
  const a = records(), b = records(2);
  f.transfer.handleStatus(blockStatus(a, 1, 0, 4)); f.transfer.handleData(fragment(a)); await flush();
  assert.equal(f.writes.filter(w => w.opcode === 2).length, 0);
  assert.equal(f.progress.at(-1)?.phase, 'saving');
  complete(); await flush(); assert.deepEqual(f.writes.at(-1), { opcode: 2, token: 1, cursor: 2 });
  f.transfer.handleStatus(blockStatus(a, 1, 0, 4)); // ACK still in flight, old block repeated
  f.transfer.handleStatus(blockStatus(b, 1, 2, 4));
  f.transfer.handleData(fragment(a)); // late first block data must not occupy second assembler
  f.transfer.handleData(fragment(b, 1, 2)); await flush(); complete(); await flush();
  assert.deepEqual(f.writes.at(-1), { opcode: 2, token: 1, cursor: 4 });
  await finish(f, 4); await initial;
  assert.equal((await f.store.ensureSession(identity)).recordCount, 4); assert.equal(f.errors.length, 0);
});

test('GAP without READY is committed before ACK and remaining snapshot determines target', async t => {
  const f = fixture(); t.after(() => f.transfer.cancel()); const initial = f.start(); await flush();
  f.transfer.handleStatus(status(4, 1, 0, 50)); await flush();
  assert.deepEqual(f.writes.at(-1), { opcode: 2, token: 1, cursor: 50 });
  f.transfer.handleStatus(status(4, 1, 0, 50)); // lost ACK, matching gap retry
  const bytes = records(50); f.transfer.handleStatus(blockStatus(bytes, 1, 50)); f.transfer.handleData(fragment(bytes, 1, 50)); await flush();
  await finish(f, 52); await initial;
  assert.equal(f.progress.at(-1)?.missingCount, 50); assert.equal(f.progress.at(-1)?.recordCount, 2);
});

test('CRC or changed retry fails without acknowledging corrupt bytes or enabling live', async t => {
  const f = fixture(); t.after(() => f.transfer.cancel()); const initial = f.start(); await flush();
  const bytes = records(); f.transfer.handleStatus(blockStatus(bytes)); bytes[16] ^= 1; f.transfer.handleData(fragment(bytes));
  await assert.rejects(initial, /checksum/);
  assert.equal(f.writes.some(w => w.opcode === 2 || w.opcode === 3), false);
  const g = fixture(); t.after(() => g.transfer.cancel()); const second = g.start(); await flush();
  const original = records(); g.transfer.handleStatus(blockStatus(original)); g.transfer.handleData(fragment(original)); await flush();
  const changed = blockStatus(original); changed.setUint32(12, 1, true); g.transfer.handleStatus(changed);
  await assert.rejects(second, /changed after saving/);
});

test('storage failure and disconnect during commit never ACK; reconnect resumes committed cursor', async t => {
  const f = fixture(); t.after(() => f.transfer.cancel());
  f.store.commitBlock = async () => { throw new Error('QuotaExceededError'); };
  const initial = f.start(); await flush(); const bytes = records();
  f.transfer.handleStatus(blockStatus(bytes)); f.transfer.handleData(fragment(bytes));
  await assert.rejects(initial, /Quota/); assert.equal(f.writes.some(w => w.opcode === 2 || w.opcode === 3), false);
  const store = new MemoryHistoryStore(), g = fixture(store); t.after(() => g.transfer.cancel());
  const original = store.commitBlock.bind(store); let complete!: () => void;
  store.commitBlock = async (...args) => { await new Promise<void>(resolve => { complete = resolve; }); return original(...args); };
  const pending = g.start(); await flush(); g.transfer.handleStatus(blockStatus(bytes)); g.transfer.handleData(fragment(bytes)); await flush();
  g.transfer.cancel(); complete(); await flush(); await assert.rejects(pending, /cancelled/);
  assert.equal(g.writes.some(w => w.opcode === 2), false);
  const resumed = fixture(store); t.after(() => resumed.transfer.cancel()); const resumedInitial = resumed.start(); await flush();
  assert.deepEqual(resumed.writes[0], { opcode: 1, token: 1, cursor: 2 });
  await finish(resumed, 2); await resumedInitial;
});

test('lost LIVE status is recovered by idempotent command and read fallback; no live before response', async t => {
  const f = fixture(); t.after(() => f.transfer.cancel()); const initial = f.start(); let ready = false; void initial.then(() => { ready = true; }); await flush();
  f.transfer.handleStatus(status(3)); await flush(); assert.equal(ready, false);
  f.setStatus(status(5, 1, 0, 10, 131072)); f.time(3001); f.transfer.tick(); await flush(); await initial;
  assert.equal(f.writes.filter(w => w.opcode === 3).length, 2); assert.equal(ready, true);
});

test('repeated END status cannot indefinitely postpone a missing LIVE timeout', async t => {
  const f = fixture(); t.after(() => f.transfer.cancel()); const initial = f.start(); await flush();
  f.transfer.handleStatus(status(3)); await flush();
  for (const time of [3001, 6002, 9003, 12004, 15005, 18006]) { f.time(time); f.transfer.handleStatus(status(3)); await flush(); }
  f.time(20001); f.transfer.tick(); await assert.rejects(initial, /stopped responding/);
});

test('background POLL is single flight and starts at the durable snapshot, including transfer tail', async t => {
  const f = fixture(); t.after(() => f.transfer.cancel()); const initial = f.start(); await flush(); await finish(f, 0); await initial;
  await new Promise(resolve => setTimeout(resolve, 15)); assert.equal(f.writes.some(w => w.opcode === 4), false);
  f.transfer.enablePolling(); await new Promise(resolve => setTimeout(resolve, 15));
  assert.deepEqual(f.writes.at(-1), { opcode: 4, token: 2, cursor: 0 });
  await new Promise(resolve => setTimeout(resolve, 15)); assert.equal(f.writes.filter(w => w.opcode === 4).length, 1);
  const bytes = records(); f.transfer.handleStatus(blockStatus(bytes, 2)); f.transfer.handleData(fragment(bytes, 2)); await flush();
  await finish(f, 2, 2); await new Promise(resolve => setTimeout(resolve, 15));
  assert.deepEqual(f.writes.at(-1), { opcode: 4, token: 3, cursor: 2 });
});

test('no PSRAM is explicit live-only and never opens storage or starts polling', async t => {
  const f = fixture(undefined, 0); t.after(() => f.transfer.cancel());
  f.store.ensureSession = async () => { throw new Error('must not touch storage'); };
  const initial = f.start(); await flush(); f.transfer.handleStatus(status(1, 1, 0, 0, 0, 0, 0));
  f.transfer.handleStatus(status(3, 1, 0, 0, 0, 0, 0)); await flush(); f.transfer.handleStatus(status(5, 1, 0, 0, 0, 0, 0)); await initial;
  assert.equal(f.progress.at(-1)?.phase, 'live-only'); f.transfer.enablePolling(); await new Promise(resolve => setTimeout(resolve, 15));
  assert.equal(f.writes.some(w => w.opcode === 4), false);
});

test('sequence exhaustion still downloads the final tail before stopping background polls', async t => {
  const f = fixture(); t.after(() => f.transfer.cancel());
  const saved = await f.store.ensureSession(identity); f.store.sessions.set(identity.key, { ...saved, nextSequence: 0xfffffffe });
  const initial = f.start(); await flush();
  f.transfer.handleStatus(status(3, 1, 0xfffffffe)); await flush();
  f.transfer.handleStatus(status(5, 1, 0xfffffffe, 0xffffffff, 131072, 0, 7)); await initial;
  f.transfer.enablePolling(); await new Promise(resolve => setTimeout(resolve, 15));
  assert.deepEqual(f.writes.at(-1), { opcode: 4, token: 2, cursor: 0xfffffffe });
  const bytes = records(0xfffffffe, 1); f.transfer.handleStatus(blockStatus(bytes, 2, 0xfffffffe));
  f.transfer.handleData(fragment(bytes, 2, 0xfffffffe)); await flush();
  f.transfer.handleStatus(status(3, 2, 0xffffffff)); await flush();
  f.transfer.handleStatus(status(5, 2, 0xffffffff, 0xffffffff, 131072, 0, 7));
  await new Promise(resolve => setTimeout(resolve, 15));
  assert.equal(f.writes.filter(write => write.opcode === 4).length, 1);
  assert.equal(f.progress.at(-1)?.cursor, 0xffffffff);
});

test('timeout, bad cursor and premature END fail clearly without silent live', async t => {
  for (const mode of ['timeout', 'cursor', 'end'] as const) {
    const f = fixture(); t.after(() => f.transfer.cancel()); const initial = f.start(); await flush();
    if (mode === 'timeout') { f.time(20001); f.transfer.tick(); }
    if (mode === 'cursor') f.transfer.handleStatus(status(6, 1, 1));
    if (mode === 'end') f.transfer.handleStatus(status(3, 1, 10));
    await assert.rejects(initial, mode === 'timeout' ? /stopped responding/ : mode === 'cursor' ? /saved history cursor/ : /before all records/);
    assert.equal(f.writes.some(w => w.opcode === 3), false);
  }
});
