import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { decodeCatalog, decodeStatus, encodeCommand } from './scenarioProtocol';

// Bytes generated from the firmware's own structs by Software/tests/scenario_fixture.cpp.
const fixture = JSON.parse(readFileSync(new URL('../../../tests/scenario-protocol-fixture.json', import.meta.url), 'utf8'));
const view = (hex: string) => new DataView(Uint8Array.from(hex.match(/../g)!.map(b => parseInt(b, 16))).buffer);

test('catalog decodes the firmware-compiled scenarios', () => {
  const catalog = decodeCatalog(view(fixture.catalog.hex));
  assert.equal(catalog.catalogCrc, fixture.catalog.crc);
  assert.equal(catalog.entries.length, fixture.catalog.entries.length);
  catalog.entries.forEach((entry, i) => {
    const expected = fixture.catalog.entries[i];
    assert.equal(entry.id, expected.id);
    assert.equal(entry.name, expected.name);
    assert.equal(entry.irSamples, expected.irSamples);
    assert.equal(entry.imuSamples, expected.imuSamples);
    assert.equal(entry.spo2Samples, expected.spo2Samples);
    assert.equal(entry.contentCrc32, expected.contentCrc32);
    assert.equal(entry.durationS, 110);
  });
});

test('commands match the firmware layout byte for byte', () => {
  const start = fixture.commands.start;
  const bytes = encodeCommand('start', start.token, start.scenarioId, fixture.catalog.crc);
  assert.equal(Buffer.from(bytes).toString('hex'), start.hex);
  assert.equal(Buffer.from(encodeCommand('abort', fixture.commands.abort.token)).toString('hex'), fixture.commands.abort.hex);
  assert.throws(() => encodeCommand('start', 0, 1, 0));
});

test('status packets decode state, reason, scores and flags', () => {
  for (const expected of fixture.statuses) {
    const s = decodeStatus(view(expected.hex));
    assert.equal(s.state, expected.state);
    assert.equal(s.reason, expected.reason);
    assert.equal(s.scenarioId, expected.scenarioId);
    assert.equal(s.remainingMs, expected.remainingMs);
    assert.equal(s.pulseScore, expected.pulseScore === null ? null : expected.pulseScore / 10000);
    assert.equal(s.oxygenConcern, expected.oxygenConcern === null ? null : expected.oxygenConcern / 10000);
    assert.equal(s.pulseAlert, expected.pulseAlert);
    assert.equal(s.oxygenAlert, expected.oxygenAlert);
    assert.equal(s.imuGateOpen, expected.imuGateOpen);
    assert.equal(s.triggerAccepted, expected.triggerAccepted);
    assert.ok(s.scenariosCompiled);
  }
  assert.match(decodeStatus(view(fixture.statuses[1].hex)).reasonText, /Battery/);
});

test('malformed packets are rejected', () => {
  assert.throws(() => decodeStatus(new DataView(new ArrayBuffer(19))));
  assert.throws(() => decodeCatalog(view('02000000')));
  assert.throws(() => decodeCatalog(view('01010000')));
});
