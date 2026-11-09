import assert from 'node:assert/strict';
import test from 'node:test';
import { defaultPreferences, parsePreferences } from './preferences';

test('old preferences retain their baselines and receive body measurement defaults', () => {
  assert.deepEqual(parsePreferences('{"heartRate":55,"maxDepth":4.2}'), {
    ...defaultPreferences, heartRate: 55, maxDepth: 4.2,
  });
});

test('all four settings survive a save/read round trip, including range boundaries', () => {
  for (const values of [
    { heartRate: 20, maxDepth: 0.1, weightKg: 20, heightCm: 80 },
    { heartRate: 140, maxDepth: 10, weightKg: 300, heightCm: 250 },
    { heartRate: 64, maxDepth: 2.5, weightKg: 82.3, heightCm: 183 },
  ]) assert.deepEqual(parsePreferences(JSON.stringify(values)), values);
});

test('invalid new measurements fall back without discarding saved baselines', () => {
  for (const measurement of [null, '100', -1, 999, {}]) {
    assert.deepEqual(parsePreferences(JSON.stringify({ heartRate: 60, maxDepth: 3, weightKg: measurement, heightCm: measurement })), {
      ...defaultPreferences, heartRate: 60, maxDepth: 3,
    });
  }
});

test('corrupt storage or invalid original baselines are rejected', () => {
  for (const raw of [null, '{', 'null', '[]', '{"heartRate":19,"maxDepth":2}', '{"heartRate":70,"maxDepth":0}']) {
    assert.equal(parsePreferences(raw), null);
  }
});
