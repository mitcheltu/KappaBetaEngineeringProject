// Verify the Swift XCTest fixture against the current WebApp decoders on any
// Node host. This checks shared wire expectations; it does not execute Swift.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { createRequire } from 'node:module';

const web = new URL('../../WebApp/', import.meta.url);
const require = createRequire(new URL('package.json', web));
const ts = require('typescript');
const compile = (source) => ts.transpileModule(source, {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ESNext },
}).outputText;
const moduleURL = source => `data:text/javascript;base64,${Buffer.from(source).toString('base64')}`;
const telemetryURL = moduleURL(compile(await readFile(new URL('src/services/telemetryProtocol.ts', web), 'utf8')));
const historySource = compile(await readFile(new URL('src/services/historyProtocol.ts', web), 'utf8'))
  .replace(/from ['"]\.\/telemetryProtocol['"]/g, `from ${JSON.stringify(telemetryURL)}`);
const history = await import(moduleURL(historySource));
const fixtureURL = new URL('../Tests/WearableCoreTests/Fixtures/history-protocol-fixture.json', import.meta.url);
const canonicalURL = new URL('../../testsAndDemo/tests/history-protocol-fixture.json', import.meta.url);
const fixtureText = await readFile(fixtureURL, 'utf8');
assert.deepEqual(JSON.parse(fixtureText), JSON.parse(await readFile(canonicalURL, 'utf8')), 'Swift fixture drifted from firmware fixture');
const fixture = JSON.parse(fixtureText), expected = fixture.expected;
const bytes = hex => Uint8Array.from(Buffer.from(hex, 'hex'));
const view = hex => new DataView(bytes(hex).buffer);

const identity = history.decodeIdentity(view(fixture.identityHex));
assert.equal(identity.deviceId, expected.deviceId);
assert.equal(identity.bootId, expected.bootId);
const status = history.decodeHistoryStatus(view(fixture.statusHex));
assert.equal(status.first, expected.first);
assert.equal(status.end, expected.end);
assert.equal(status.token, expected.token);
const raw = bytes(fixture.recordBlockHex);
assert.equal(history.crc32(raw), fixture.crc32);
assert.deepEqual(history.historyCommand(history.HistoryCommand.ACK, expected.token, expected.end), bytes(fixture.commandHex));
const assembler = new history.HistoryBlockAssembler(status);
for (const hex of [...fixture.dataFragmentsHex].reverse()) {
  assembler.accept(view(hex));
  assert.equal(assembler.accept(view(hex)).added, 0, 'Duplicate fragment counted twice');
}
assert.deepEqual(assembler.bytes, raw);
const records = history.decodeHistoryBlock(raw, expected.first);
assert.deepEqual(records.map(record => record.timeUs), expected.timeUs);
const values = records.map(history.recordCsvValues);
assert.deepEqual([values[0].pressure_pa, values[0].depth_m, values[0].temperature_c, values[0].surface_pa],
  [expected.depth.pressurePa, expected.depth.depthM, expected.depth.temperatureC, expected.depth.surfacePa]);
assert.deepEqual(['x', 'y', 'z'].map(axis => values[1][`accel_${axis}_g`]), expected.imu.accelerationG);
assert.deepEqual(['x', 'y', 'z'].map(axis => values[1][`gyro_${axis}_dps`]), expected.imu.angularVelocityDps);
assert.deepEqual([values[2].red, values[2].infrared, values[2].heart_rate_bpm, values[2].spo2_percent, values[2].estimated_sample_time],
  [expected.ppg.red, expected.ppg.infrared, expected.ppg.heartRateBpm, expected.ppg.spo2Percent, expected.ppg.estimatedSampleTime]);
assert.deepEqual([values[3].battery_v, values[3].remaining_ms, values[3].ppg_overflows, values[3].sensor_errors],
  [expected.system.batteryV, expected.system.remainingMs, expected.system.ppgOverflows, expected.system.sensorErrors]);
assert.equal(values[4].surface_pa, expected.surfaceZeroPa);
console.log('PASS: Swift fixture matches firmware fixture and current WebApp identity, command, CRC, fragments, 5 record kinds, exact timestamps and CSV values. Swift execution still requires swift test.');
