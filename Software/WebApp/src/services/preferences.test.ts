import assert from 'node:assert/strict';
import test from 'node:test';
import { defaultPreferences, displayWeight, weightToKg, parsePreferences } from './preferences';

test('old preferences retain their baselines and receive body measurement defaults', () => {
  assert.deepEqual(parsePreferences('{"heartRate":55,"maxDepth":4.2}'), {
    ...defaultPreferences, heartRate: 55, maxDepth: 4.2,
  });
});

test('all four settings survive a save/read round trip, including range boundaries', () => {
  for (const values of [
    { heartRate: 20, maxDepth: 0.1, weightKg: 20, heightCm: 80 },
    { heartRate: 140, maxDepth: 10, weightKg: 150, heightCm: 250 },
    { heartRate: 64, maxDepth: 2.5, weightKg: 82.3, heightCm: 183 },
  ]) assert.deepEqual(parsePreferences(JSON.stringify(values)), { ...values, weightUnit: 'kg' });
});

test('pounds convert correctly and retain precision when saved', () => {
  assert.equal(displayWeight(70, 'lbs'), 154.3);
  const weightKg = weightToKg(180.5, 'lbs');
  assert.ok(Math.abs(weightKg - 81.873422785) < 1e-9);
  const saved = parsePreferences(JSON.stringify({ ...defaultPreferences, weightKg, weightUnit: 'lbs' }));
  assert.equal(saved?.weightUnit, 'lbs');
  assert.equal(saved?.weightKg, weightKg);
  assert.equal(displayWeight(saved!.weightKg, 'lbs'), 180.5);
  assert.equal(displayWeight(saved!.weightKg, 'kg'), 81.9);
});

test('both unit ranges reach the same weight limits', () => {
  for (const unit of ['kg', 'lbs'] as const) {
    assert.equal(weightToKg(displayWeight(20, unit), unit), 20);
    assert.equal(weightToKg(displayWeight(150, unit), unit), 150);
    assert.equal(weightToKg(displayWeight(300, unit), unit), 150);
  }
  assert.equal(displayWeight(150, 'lbs'), 330.7);
  assert.equal(parsePreferences(JSON.stringify({ ...defaultPreferences, weightKg: 200 }))?.weightKg, 150);
  assert.equal(parsePreferences(JSON.stringify({ ...defaultPreferences, weightUnit: 'invalid' }))?.weightUnit, 'kg');
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
