import assert from 'node:assert/strict';
import test from 'node:test';
import { defaultPreferences } from './preferences';
import { encodeProfile, verifyProfileReadback } from './profileProtocol';

test('profile v1 matches the shared Arduino/iOS default packet', () => {
  assert.deepEqual([...encodeProfile(defaultPreferences)], [1, 1, 70, 0, 208, 7, 188, 2, 170, 0, 0, 0]);
});

test('metric wire values preserve boundaries and round converted pounds and depth', () => {
  for (const preferences of [
    { ...defaultPreferences, heartRate: 20, maxDepth: 0.1, weightKg: 20, heightCm: 80 },
    { ...defaultPreferences, heartRate: 140, maxDepth: 10, weightKg: 150, heightCm: 250 },
    { ...defaultPreferences, maxDepth: 2.3456, weightKg: 81.873422785, weightUnit: 'lbs' as const },
  ]) {
    const view = new DataView(encodeProfile(preferences).buffer);
    assert.equal(view.getUint16(2, true), preferences.heartRate);
    assert.equal(view.getUint16(4, true), Math.round(preferences.maxDepth * 1000));
    assert.equal(view.getUint16(6, true), Math.round(preferences.weightKg * 10));
    assert.equal(view.getUint16(8, true), preferences.heightCm);
    assert.equal(view.getUint16(10, true), 0);
  }
});

test('profile validation rejects original out-of-range values before rounding', () => {
  const invalid = {
    heartRate: [19, 141, 70.5, NaN, Infinity],
    maxDepth: [0.0999, 10.00001, NaN, Infinity],
    weightKg: [19.9999, 150.0001, NaN, Infinity],
    heightCm: [79, 251, 170.5, NaN, Infinity],
  };
  for (const [key, values] of Object.entries(invalid)) {
    for (const value of values) assert.throws(() => encodeProfile({ ...defaultPreferences, [key]: value }), /invalid/);
  }
});

test('readback requires exact accepted bytes and respects DataView offsets', () => {
  const expected = encodeProfile(defaultPreferences);
  const padded = new Uint8Array(20); padded.set(expected, 3);
  verifyProfileReadback(expected, new DataView(padded.buffer, 3, 12));
  for (const size of [0, 11, 13]) assert.throws(() => verifyProfileReadback(expected, new DataView(new ArrayBuffer(size))), /did not confirm/);
  for (let index = 0; index < expected.length; index++) {
    const changed = expected.slice(); changed[index] ^= 1;
    assert.throws(() => verifyProfileReadback(expected, new DataView(changed.buffer)), /did not confirm/);
  }
});
