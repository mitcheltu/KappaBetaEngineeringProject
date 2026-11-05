import assert from 'node:assert/strict';
import test from 'node:test';
import { readFileSync } from 'node:fs';
import { HistoryBlockAssembler, crc32, decodeHistoryBlock, decodeHistoryStatus, decodeIdentity, historyCommand, recordCsvValues } from './historyProtocol';
import { historyCsvRow } from './historyExport';
import { newSession } from './historyStore';
import { blockStatus, fragment, identity, records, status } from './historyTestFixtures';

test('history identity handles offsets and reserved/version bytes; commands preserve terminal cursor', () => {
  const padded = new Uint8Array(30), view = new DataView(padded.buffer, 5, 20);
  view.setUint8(0, 1); view.setUint8(1, 32); view.setUint8(2, 3);
  for (let i = 4; i < 18; i++) view.setUint8(i, i);
  assert.deepEqual(decodeIdentity(view), { deviceId: '040506070809', bootId: '0a0b0c0d0e0f1011', key: '040506070809:0a0b0c0d0e0f1011', flags: 3 });
  view.setUint8(3, 1); assert.throws(() => decodeIdentity(view), /reserved/);
  view.setUint8(0, 2); assert.throws(() => decodeIdentity(view), /Unsupported/);
  assert.throws(() => decodeHistoryStatus(new DataView(new ArrayBuffer(19))), /Unsupported/);
  assert.throws(() => decodeHistoryStatus(status(7)), /Unknown/);
  const encoded = historyCommand(2, 65535, 0xffffffff);
  assert.deepEqual([...encoded], [2, 1, 255, 255, 255, 255, 255, 255]);
  for (const cursor of [-1, 0x100000000, 1.5]) assert.throws(() => historyCommand(1, 1, cursor));
  assert.throws(() => historyCommand(1, 0, 0));
});

test('default MTU fragments assemble out of order, check duplicates, bounds and IEEE CRC', () => {
  assert.equal(crc32(new TextEncoder().encode('123456789')), 0xcbf43926);
  const bytes = records(100, 64), assembler = new HistoryBlockAssembler(decodeHistoryStatus(blockStatus(bytes, 9, 100)));
  for (let offset = 2040; offset >= 0; offset -= 12) assembler.accept(fragment(bytes, 9, 100, offset, Math.min(12, bytes.length - offset)));
  assert.deepEqual(assembler.bytes, bytes);
  assert.deepEqual(assembler.accept(fragment(bytes, 9, 100, 0, 12)), { complete: true, added: 0 });
  const bad = fragment(bytes, 9, 100, 0, 12); bad.setUint8(8, 255);
  assert.throws(() => assembler.accept(bad), /Conflicting/);
  assert.throws(() => assembler.accept(fragment(bytes, 9, 100, 2048, 1)), /exceeds/);
  assert.throws(() => assembler.accept(new DataView(new ArrayBuffer(8))), /Truncated/);
  const corrupt = new HistoryBlockAssembler({ ...decodeHistoryStatus(blockStatus(bytes)), detail: 123 });
  assert.throws(() => corrupt.accept(fragment(bytes)), /checksum/);
  assert.throws(() => new HistoryBlockAssembler({ ...decodeHistoryStatus(blockStatus(bytes)), count: 65 }), /bounds/);
});

test('records preserve 64-bit time and sample ordering while rejecting invalid sequences and formats', () => {
  const bytes = records(); const view = new DataView(bytes.buffer); view.setBigUint64(40, 100n, true);
  const decoded = decodeHistoryBlock(bytes, 0);
  assert.equal(decoded[0].timeUs, '10000000000000000'); assert.equal(decoded[1].timeUs, '100');
  assert.equal(recordCsvValues(decoded[0]).accel_x_g, 1);
  view.setUint16(6, 1, true); assert.throws(() => decodeHistoryBlock(bytes, 0), /format/); view.setUint16(6, 0, true);
  view.setUint32(32, 9, true); assert.throws(() => decodeHistoryBlock(bytes, 0), /Nonconsecutive/);
  assert.throws(() => decodeHistoryBlock(records(0xffffffff, 1), 0xffffffff), /Nonconsecutive/);
  assert.equal(decodeHistoryBlock(records(0xfffffffe, 1), 0xfffffffe)[0].sequence, 0xfffffffe);
});

test('CSV distinguishes PPG estimated sample time, invalid values, baseline events, and explicit gaps', () => {
  const bytes = records(0, 1, 3), view = new DataView(bytes.buffer);
  view.setUint8(5, 0x87); view.setUint32(16, 50000, true); view.setUint32(20, 60000, true);
  view.setUint16(24, 720, true); view.setUint16(26, 9800, true);
  const ppg = recordCsvValues(decodeHistoryBlock(bytes, 0)[0]);
  assert.equal(ppg.estimated_sample_time, true); assert.equal(ppg.heart_rate_bpm, 72); assert.equal(ppg.red, 50000);
  view.setUint8(5, 128); assert.equal(recordCsvValues(decodeHistoryBlock(bytes, 0)[0]).red, null);
  const event = records(0, 1, 5); new DataView(event.buffer).setFloat32(16, 101325, true);
  assert.equal(recordCsvValues(decodeHistoryBlock(event, 0)[0]).surface_pa, 101325);
  const csv = historyCsvRow(newSession(identity), { type: 'gap', sessionKey: identity.key, sequence: 10, end: 30, importedAt: '2026-11-05T00:00:00Z' });
  assert.match(csv, /^gap,001122334455,0102030405060708,10,30,/);
});

test('actual C++ wire fixture agrees on identity, CRC, all record kinds, timestamps and ACK', () => {
  const fixture = JSON.parse(readFileSync(new URL('../../../tests/history-protocol-fixture.json', import.meta.url), 'utf8'));
  const bytes = (hex: string) => Uint8Array.from(Buffer.from(hex, 'hex'));
  const view = (hex: string) => new DataView(bytes(hex).buffer);
  const expected = fixture.expected, identity = decodeIdentity(view(fixture.identityHex));
  assert.equal(identity.deviceId, expected.deviceId); assert.equal(identity.bootId, expected.bootId);
  const status = decodeHistoryStatus(view(fixture.statusHex)), assembler = new HistoryBlockAssembler(status);
  for (const hex of fixture.dataFragmentsHex) assembler.accept(view(hex));
  assert.deepEqual(assembler.bytes, bytes(fixture.recordBlockHex)); assert.equal(crc32(assembler.bytes), fixture.crc32);
  assert.deepEqual(historyCommand(2, expected.token, expected.end), bytes(fixture.commandHex));
  const records = decodeHistoryBlock(assembler.bytes, expected.first);
  assert.deepEqual(records.map(record => record.timeUs), expected.timeUs);
  const [depth, imu, ppg, system, zero] = records.map(recordCsvValues);
  assert.equal(depth.pressure_pa, expected.depth.pressurePa); assert.equal(depth.depth_m, expected.depth.depthM);
  assert.equal(depth.temperature_c, expected.depth.temperatureC); assert.equal(depth.surface_pa, expected.depth.surfacePa);
  assert.deepEqual([imu.accel_x_g, imu.accel_y_g, imu.accel_z_g], expected.imu.accelerationG);
  assert.deepEqual([imu.gyro_x_dps, imu.gyro_y_dps, imu.gyro_z_dps], expected.imu.angularVelocityDps);
  assert.equal(ppg.red, expected.ppg.red); assert.equal(ppg.infrared, expected.ppg.infrared);
  assert.equal(ppg.heart_rate_bpm, expected.ppg.heartRateBpm); assert.equal(ppg.spo2_percent, expected.ppg.spo2Percent);
  assert.equal(ppg.estimated_sample_time, expected.ppg.estimatedSampleTime);
  assert.equal(system.battery_v, expected.system.batteryV); assert.equal(system.remaining_ms, expected.system.remainingMs);
  assert.equal(system.ppg_overflows, expected.system.ppgOverflows); assert.equal(system.sensor_errors, expected.system.sensorErrors);
  assert.equal(zero.surface_pa, expected.surfaceZeroPa);
});
