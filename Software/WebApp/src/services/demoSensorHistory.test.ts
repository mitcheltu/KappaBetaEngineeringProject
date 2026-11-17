import assert from 'node:assert/strict';
import test from 'node:test';
import { readFileSync } from 'node:fs';
import { decodePacket } from './telemetryProtocol';
import { decodeIdentity, decodeHistoryStatus, HistoryBlockAssembler, decodeHistoryBlock, recordCsvValues } from './historyProtocol';
import { createHistoryTransfer } from './historyTransfer';
import { MemoryHistoryStore, command, flush, status } from './historyTestFixtures';

const fixture = JSON.parse(readFileSync(new URL('../../../testsAndDemo/tests/demo-sensor-history-fixture.json', import.meta.url), 'utf8'));
const view = (hex: string) => {
  const bytes = Uint8Array.from(Buffer.from(hex, 'hex'));
  return new DataView(bytes.buffer);
};

test('real demo firmware encoders produce compatible live readings and sensor history, without scenario records', () => {
  assert.equal(decodeIdentity(view(fixture.identityHex)).flags, 1);
  const depth = decodePacket('depth', view(fixture.live.depth));
  assert.ok(Math.abs(depth.depthM! - 0.25) < 0.00001);
  assert.equal(depth.temperatureC, 22.5);
  const imu = decodePacket('imu', view(fixture.live.imu));
  assert.deepEqual(imu.accelerationG, [-1, 0.5, 8]);
  assert.deepEqual(imu.angularVelocityDps, [-2000, 2000, 123.4]);
  const ppg = decodePacket('ppg', view(fixture.live.ppg));
  assert.equal(ppg.heartRateBpm, 72); assert.equal(ppg.spo2Percent, 98);
  assert.equal(ppg.red, 123456); assert.equal(ppg.infrared, 234567);
  const system = decodePacket('system', view(fixture.live.system));
  assert.equal(system.batteryV, 3.789); assert.equal(system.remainingMs, 9876);
  assert.equal(system.vibrationMotor, true); assert.equal(system.vibrationOn, true);
  assert.equal(system.countdownActive, true); assert.equal(system.pumpOn, false);

  const assembler = new HistoryBlockAssembler(decodeHistoryStatus(view(fixture.statusHex)));
  for (const hex of fixture.dataFragmentsHex) assembler.accept(view(hex));
  assert.deepEqual(assembler.bytes, Uint8Array.from(Buffer.from(fixture.recordBlockHex, 'hex')));
  const records = decodeHistoryBlock(assembler.bytes, 100);
  assert.deepEqual(records.map(r => r.kind), [1, 2, 3, 4, 5, 3, 3, 1, 2, 5, 4]);
  assert.equal(records[0].timeUs, '9007199254740993');
  const values = records.map(recordCsvValues);
  assert.equal(values[0].depth_m, depth.depthM); assert.equal(values[0].surface_pa, 101325);
  assert.equal(values[1].gyro_x_dps, -2000);
  assert.equal(values[2].red, ppg.red); assert.equal(values[2].heart_rate_bpm, ppg.heartRateBpm);
  assert.equal(values[2].estimated_sample_time, true);
  assert.equal(values[3].vibration_on, true); assert.equal(values[3].battery_v, system.batteryV);
  assert.equal(values[4].surface_pa, 101325);
  assert.equal(values[5].red, 123456); assert.equal(values[5].heart_rate_bpm, null);
  assert.equal(values[5].spo2_percent, null);
  assert.equal(values[6].red, null); assert.equal(values[6].estimated_sample_time, false);
  assert.equal(values[7].pressure_pa, null); assert.equal(values[7].depth_m, null);
  assert.equal(values[8].accel_x_g, null); assert.equal(values[9].surface_pa, null);
  assert.equal(values[10].battery_v, null); assert.equal(values[10].pump_enabled, false);
});

test('firmware-generated demo history saves before ACK, enables live and resumes the same boot', async t => {
  const identity = decodeIdentity(view(fixture.identityHex));
  const store = new MemoryHistoryStore();
  await store.commitGap(identity, 0, 100); // Earlier ring contents were overwritten.
  const writes: ReturnType<typeof command>[] = [];
  const makeTransfer = () => createHistoryTransfer({ identity, store, initialToken: 41,
    write: async bytes => { writes.push(command(bytes)); },
    readStatus: async () => status(1, 0, 100, 111, 16, 0, 1),
    onError: error => { throw error; }, onProgress: () => {},
  });
  const transfer = makeTransfer(); t.after(() => transfer.cancel());
  let releaseCommit!: () => void;
  const commit = store.commitBlock.bind(store);
  store.commitBlock = async (...args) => {
    await new Promise<void>(resolve => { releaseCommit = resolve; });
    return commit(...args);
  };
  const initial = transfer.start(); await flush();
  assert.deepEqual(writes.at(-1), { opcode: 1, token: 42, cursor: 100 });
  transfer.handleStatus(view(fixture.statusHex));
  for (const hex of fixture.dataFragmentsHex) transfer.handleData(view(hex));
  await flush();
  assert.equal(writes.some(w => w.opcode === 2), false);
  releaseCommit(); await flush();
  assert.deepEqual(writes.at(-1), { opcode: 2, token: 42, cursor: 111 });
  transfer.handleStatus(status(3, 42, 111, 111, 0, 0, 1)); await flush();
  assert.deepEqual(writes.at(-1), { opcode: 3, token: 42, cursor: 111 });
  transfer.handleStatus(status(5, 42, 111, 112, 16, 0, 1)); await initial;
  assert.equal((await store.ensureSession(identity)).recordCount, 11);
  transfer.cancel();
  const resumed = makeTransfer(); t.after(() => resumed.cancel());
  const next = resumed.start(); await flush();
  assert.deepEqual(writes.at(-1), { opcode: 1, token: 42, cursor: 111 });
  resumed.handleStatus(status(3, 42, 111, 111, 0, 0, 1)); await flush();
  resumed.handleStatus(status(5, 42, 111, 112, 16, 0, 1)); await next;
});
