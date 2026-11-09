import assert from 'node:assert/strict';
import test from 'node:test';
import { createLiveConnectionService, type BleDevice, type BluetoothAdapter, type GattCharacteristic, type GattServer } from './liveConnection';
import { decodePacket, SERVICE_UUID, TELEMETRY_UUIDS, ZERO_DEPTH_UUID } from './telemetryProtocol';
import { HISTORY_UUIDS } from './historyProtocol';
import { defaultPreferences } from './preferences';
import { encodeProfile, PROFILE_UUID, profileMatchesPreferences } from './profileProtocol';

function packet(flags = 0, uptime = 1234, sequence = 12) {
  const view = new DataView(new ArrayBuffer(20));
  view.setUint8(0, 1); view.setUint8(1, flags);
  view.setUint16(2, sequence, true); view.setUint32(4, uptime, true);
  return view;
}

test('depth packets use little-endian floats, validity, and nonzero DataView offsets', () => {
  const payload = packet(3);
  payload.setFloat32(8, 101325, true); payload.setFloat32(12, -0.125, true); payload.setFloat32(16, 21.5, true);
  const padded = new Uint8Array(28); padded.set(new Uint8Array(payload.buffer), 5);
  assert.deepEqual(decodePacket('depth', new DataView(padded.buffer, 5, 20)), {
    sequence: 12, uptimeMs: 1234, pressurePa: 101325, depthM: -0.125, temperatureC: 21.5,
  });
  payload.setUint8(1, 1);
  assert.equal(decodePacket('depth', payload).depthM, null);
  payload.setFloat32(8, Infinity, true); payload.setFloat32(16, NaN, true);
  assert.equal(decodePacket('depth', payload).pressurePa, null);
  assert.equal(decodePacket('depth', payload).temperatureC, null);
});

test('IMU scales signed axes and PPG retains unknown values as null', () => {
  const imu = packet(1);
  [-1000, 500, 0, -123, 45, 300].forEach((value, index) => imu.setInt16(8 + index * 2, value, true));
  assert.deepEqual(decodePacket('imu', imu).accelerationG, [-1, 0.5, 0]);
  assert.deepEqual(decodePacket('imu', imu).angularVelocityDps, [-12.3, 4.5, 30]);
  imu.setUint8(1, 0); assert.equal(decodePacket('imu', imu).accelerationG, null);
  const ppg = packet(7);
  ppg.setUint32(8, 50000, true); ppg.setUint32(12, 60000, true);
  ppg.setUint16(16, 725, true); ppg.setUint16(18, 9825, true);
  assert.deepEqual(decodePacket('ppg', ppg), { sequence: 12, uptimeMs: 1234, red: 50000, infrared: 60000, heartRateBpm: 72.5, spo2Percent: 98.25 });
  ppg.setUint16(16, 0xffff, true); ppg.setUint16(18, 0xffff, true);
  assert.equal(decodePacket('ppg', ppg).heartRateBpm, null); assert.equal(decodePacket('ppg', ppg).spo2Percent, null);
  ppg.setUint16(18, 10001, true); assert.equal(decodePacket('ppg', ppg).spo2Percent, null);
  ppg.setUint8(1, 0); assert.equal(decodePacket('ppg', ppg).red, null);
});

test('system packet carries status without exposing an actuator command', () => {
  const system = packet(31);
  system.setUint16(8, 4100, true); system.setUint16(10, 250, true);
  system.setUint32(12, 42, true); system.setUint32(16, 123, true);
  assert.deepEqual(decodePacket('system', system), {
    sequence: 12, uptimeMs: 1234, batteryV: 4.1, pumpEnabled: true, pumpOn: true,
    cancelPressed: true, sosPressed: true, remainingMs: 250, ppgOverflowCount: 42, sensorErrors: 123,
    vibrationOn: false, countdownActive: false, vibrationMotor: false,
  });
  system.setUint16(8, 0xffff, true); assert.equal(decodePacket('system', system).batteryV, null);
});

test('system flags distinguish a vibrating countdown from a running pump', () => {
  const system = packet(1 | 2 | 32 | 64);
  system.setUint16(10, 10000, true);
  const countdown = decodePacket('system', system);
  assert.equal(countdown.vibrationOn, true);
  assert.equal(countdown.vibrationMotor, false);
  assert.equal(countdown.countdownActive, true);
  assert.equal(countdown.pumpOn, false);
  assert.equal(countdown.remainingMs, 10000);
  system.setUint8(1, 1 | 2 | 4);
  system.setUint16(10, 0, true);
  const running = decodePacket('system', system);
  assert.equal(running.vibrationOn, false);
  assert.equal(running.countdownActive, false);
  assert.equal(running.pumpOn, true);
  assert.equal(running.remainingMs, 0);
});

test('demo motor capability preserves v1 packets and the full 60-second pump timer', () => {
  const system = packet(1 | 2 | 32 | 64 | 128);
  system.setUint16(8, 3800, true); system.setUint16(10, 10000, true);
  const vibrating = decodePacket('system', system);
  assert.equal(vibrating.vibrationMotor, true);
  assert.equal(vibrating.vibrationOn, true);
  assert.equal(vibrating.countdownActive, true);
  assert.equal(vibrating.pumpOn, false);
  system.setUint8(1, 1 | 2 | 4 | 128);
  system.setUint16(10, 60000, true);
  const pumping = decodePacket('system', system);
  assert.equal(pumping.vibrationMotor, true);
  assert.equal(pumping.vibrationOn, false);
  assert.equal(pumping.countdownActive, false);
  assert.equal(pumping.pumpOn, true);
  assert.equal(pumping.remainingMs, 60000);
  assert.equal(pumping.batteryV, 3.8);
});

test('malformed lengths and unknown protocol versions are rejected', () => {
  for (const kind of Object.keys(TELEMETRY_UUIDS) as (keyof typeof TELEMETRY_UUIDS)[]) {
    for (const size of [0, 19, 21]) assert.throws(() => decodePacket(kind, new DataView(new ArrayBuffer(size))), /length/);
    const wrong = packet(); wrong.setUint8(0, 2);
    assert.throws(() => decodePacket(kind, wrong), /version/);
  }
});

class Events {
  listeners = new Map<string, Set<() => void>>();
  addEventListener(type: string, callback: () => void) {
    if (!this.listeners.has(type)) this.listeners.set(type, new Set());
    this.listeners.get(type)!.add(callback);
  }
  removeEventListener(type: string, callback: () => void) { this.listeners.get(type)?.delete(callback); }
  emit(type: string) { for (const callback of [...this.listeners.get(type) ?? []]) callback(); }
}
class Characteristic extends Events implements GattCharacteristic {
  value: DataView = packet();
  writes: number[][] = [];
  async startNotifications() { return this; }
  async readValue() { return this.value; }
  async writeValueWithResponse(value: Uint8Array) { this.writes.push([...value]); }
  notify(value: DataView) { this.value = value; this.emit('characteristicvaluechanged'); }
}
function fixture() {
  const characteristics = Object.fromEntries([...Object.values(TELEMETRY_UUIDS), ZERO_DEPTH_UUID, ...Object.values(HISTORY_UUIDS)].map(uuid => [uuid, new Characteristic()]));
  const identity = new DataView(new ArrayBuffer(20)); identity.setUint8(0, 1); identity.setUint8(1, 32);
  characteristics[HISTORY_UUIDS.identity].value = identity; // Explicit live-only fixture needs no IndexedDB.
  characteristics[HISTORY_UUIDS.command].writeValueWithResponse = async bytes => {
    characteristics[HISTORY_UUIDS.command].writes.push([...bytes]);
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const status = new DataView(new ArrayBuffer(20)); status.setUint8(0, bytes[0] === 3 ? 5 : 3);
    status.setUint8(1, 1); status.setUint16(2, view.getUint16(2, true), true);
    if (bytes[0] === 1 || bytes[0] === 3) characteristics[HISTORY_UUIDS.status].notify(status);
  };
  const device: BleDevice & Events = Object.assign(new Events(), { name: 'KappaBetaWearable', gatt: undefined as GattServer | undefined });
  const server: GattServer = {
    connected: false,
    async connect() { this.connected = true; return this; },
    disconnect() { const wasConnected = this.connected; this.connected = false; if (wasConnected) device.emit('gattserverdisconnected'); },
    async getPrimaryService(uuid) {
      assert.equal(uuid, SERVICE_UUID);
      return { async getCharacteristic(characteristicUuid: string) {
        if (!characteristics[characteristicUuid]) throw Object.assign(new Error('Missing characteristic'), { name: 'NotFoundError' });
        return characteristics[characteristicUuid];
      } };
    },
  };
  device.gatt = server;
  const bluetooth: BluetoothAdapter = { async requestDevice(options) {
    assert.deepEqual(options, { filters: [{ services: [SERVICE_UUID] }] }); return device;
  } };
  return { bluetooth, device, server, characteristics };
}

function profileFixture() {
  const f = fixture();
  const profile = new Characteristic();
  profile.value = new DataView(new ArrayBuffer(12)); profile.value.setUint8(0, 1);
  profile.writeValueWithResponse = async bytes => {
    profile.writes.push([...bytes]); profile.value = new DataView(bytes.slice().buffer);
  };
  f.characteristics[PROFILE_UUID] = profile;
  return { ...f, profile };
}

test('live connection receives four packets, expires each stream, and ignores duplicate refreshes', async t => {
  const f = fixture(); let now = 0;
  const service = createLiveConnectionService({ bluetooth: f.bluetooth, now: () => now });
  t.after(() => service.dispose());
  await service.connect(); assert.equal(service.getSnapshot().status, 'connected');
  assert.ok(service.getSnapshot().telemetry.depth);
  assert.ok(service.getSnapshot().telemetry.imu);
  assert.ok(service.getSnapshot().telemetry.ppg);
  assert.ok(service.getSnapshot().telemetry.system);
  now = 3000; service.refreshStaleness();
  assert.equal(service.getSnapshot().telemetry.depth?.stale, true);
  f.characteristics[TELEMETRY_UUIDS.depth].notify(packet());
  assert.equal(service.getSnapshot().telemetry.depth?.stale, true);
  f.characteristics[TELEMETRY_UUIDS.depth].notify(packet(3, 5000, 13));
  assert.equal(service.getSnapshot().telemetry.depth?.stale, false);
  assert.equal(service.getSnapshot().telemetry.imu?.stale, true);
  f.characteristics[TELEMETRY_UUIDS.depth].notify(packet(3, 4000, 12));
  assert.equal(service.getSnapshot().telemetry.depth?.data.uptimeMs, 5000);
});

test('disconnect clears readings and reconnect creates fresh subscriptions', async t => {
  const f = fixture(); const service = createLiveConnectionService({ bluetooth: f.bluetooth });
  t.after(() => service.dispose());
  await service.connect(); f.server.disconnect();
  assert.equal(service.getSnapshot().status, 'lost');
  assert.deepEqual(service.getSnapshot().telemetry, { depth: null, imu: null, ppg: null, system: null });
  await service.connect(); assert.equal(service.getSnapshot().status, 'connected');
  service.disconnect(); assert.equal(service.getSnapshot().status, 'disconnected');
  assert.equal(f.characteristics[TELEMETRY_UUIDS.depth].listeners.get('characteristicvaluechanged')?.size, 0);
});

test('bad notifications clear only their sensor and surface errors', async t => {
  const f = fixture(); const service = createLiveConnectionService({ bluetooth: f.bluetooth });
  t.after(() => service.dispose());
  await service.connect();
  f.characteristics[TELEMETRY_UUIDS.ppg].notify(new DataView(new ArrayBuffer(19)));
  assert.equal(service.getSnapshot().telemetry.ppg, null);
  assert.ok(service.getSnapshot().telemetry.depth);
  assert.match(service.getSnapshot().message, /packet length/);
});

test('surface zero sends exactly one command byte and clears the previous depth', async t => {
  const f = fixture(); const depth = packet(3); depth.setFloat32(12, 2, true);
  f.characteristics[TELEMETRY_UUIDS.depth].value = depth;
  const service = createLiveConnectionService({ bluetooth: f.bluetooth }); t.after(() => service.dispose());
  await service.connect(); assert.equal(service.getSnapshot().telemetry.depth?.data.depthM, 2);
  await service.zeroDepth();
  assert.deepEqual(f.characteristics[ZERO_DEPTH_UUID].writes, [[1]]);
  assert.equal(service.getSnapshot().telemetry.depth?.data.depthM, null);
  assert.equal(service.getSnapshot().zeroPending, false);
  service.disconnect(); await service.zeroDepth();
  assert.equal(f.characteristics[ZERO_DEPTH_UUID].writes.length, 1);
});

test('cancelled selection and late chooser completion never connect', async t => {
  const f = fixture(); let resolveDevice!: (device: BleDevice) => void;
  const service = createLiveConnectionService({ bluetooth: { requestDevice: () => new Promise(resolve => { resolveDevice = resolve; }) } });
  t.after(() => service.dispose());
  const pending = service.connect(); assert.equal(service.getSnapshot().status, 'selecting');
  service.disconnect(); resolveDevice(f.device); await pending;
  assert.equal(f.server.connected, false); assert.equal(service.getSnapshot().status, 'disconnected');
  const cancelled = createLiveConnectionService({ bluetooth: { async requestDevice() { const error = new Error('Cancelled'); error.name = 'NotFoundError'; throw error; } } });
  t.after(() => cancelled.dispose()); await cancelled.connect();
  assert.equal(cancelled.getSnapshot().status, 'disconnected');
});

test('live subscriptions wait for history LIVE and missing history requires firmware update', async t => {
  const f = fixture();
  const originalWrite = f.characteristics[HISTORY_UUIDS.command].writeValueWithResponse;
  let release!: () => void;
  f.characteristics[HISTORY_UUIDS.command].writeValueWithResponse = async bytes => {
    if (bytes[0] === 1) await new Promise<void>(resolve => { release = resolve; });
    await originalWrite(bytes);
  };
  const service = createLiveConnectionService({ bluetooth: f.bluetooth }); t.after(() => service.dispose());
  const connecting = service.connect();
  while (!release) await new Promise(resolve => setImmediate(resolve));
  assert.equal(service.getSnapshot().status, 'syncing');
  assert.equal(f.characteristics[TELEMETRY_UUIDS.depth].listeners.size, 0);
  release(); await connecting;
  assert.equal(service.getSnapshot().status, 'connected');
  assert.equal(service.getSnapshot().history?.phase, 'live-only');
  service.disconnect();
  delete f.characteristics[HISTORY_UUIDS.identity];
  await service.connect();
  assert.equal(service.getSnapshot().status, 'error');
  assert.match(service.getSnapshot().message, /Firmware update required/);
});

test('cancel during initial history rejects pending work and removes subscriptions', async t => {
  const f = fixture(); f.characteristics[HISTORY_UUIDS.command].writeValueWithResponse = async () => {};
  const service = createLiveConnectionService({ bluetooth: f.bluetooth }); t.after(() => service.dispose());
  const connecting = service.connect();
  while (service.getSnapshot().status !== 'syncing') await new Promise(resolve => setImmediate(resolve));
  service.disconnect(); await connecting;
  assert.equal(service.getSnapshot().status, 'disconnected');
  for (const characteristic of Object.values(f.characteristics)) assert.equal(characteristic.listeners.get('characteristicvaluechanged')?.size ?? 0, 0);
});

test('LIVE notification before the write resolves cannot overlap subsequent GATT operations', async t => {
  const f = profileFixture(), historyCommand = f.characteristics[HISTORY_UUIDS.command];
  const originalWrite = historyCommand.writeValueWithResponse; let release!: () => void;
  historyCommand.writeValueWithResponse = async bytes => {
    await originalWrite(bytes);
    if (bytes[0] === 3) await new Promise<void>(resolve => { release = resolve; });
  };
  const service = createLiveConnectionService({ bluetooth: f.bluetooth, getSavedPreferences: () => defaultPreferences }); t.after(() => service.dispose());
  const connecting = service.connect();
  while (!release) await new Promise(resolve => setImmediate(resolve));
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(f.characteristics[TELEMETRY_UUIDS.depth].listeners.size, 0);
  assert.equal(f.profile.writes.length, 0);
  assert.equal(service.getSnapshot().status, 'syncing');
  release(); await connecting; assert.equal(service.getSnapshot().status, 'connected');
  assert.equal(service.getSnapshot().profileSync.status, 'synced');
});

test('late completion of a cancelled connect cannot disconnect a newer connection to the same device', async t => {
  const f = fixture(); let release!: () => void, calls = 0;
  f.server.connect = async () => {
    if (++calls === 1) await new Promise<void>(resolve => { release = resolve; });
    f.server.connected = true; return f.server;
  };
  const service = createLiveConnectionService({ bluetooth: f.bluetooth }); t.after(() => service.dispose());
  const old = service.connect(); while (!release) await new Promise(resolve => setImmediate(resolve));
  service.disconnect(); await service.connect(); assert.equal(service.getSnapshot().status, 'connected');
  release(); await old;
  assert.equal(f.server.connected, true); assert.equal(service.getSnapshot().status, 'connected');
});

test('profile sync uses latest saved settings after history and verifies receipt before live subscriptions', async t => {
  const f = profileFixture(); let saved = { ...defaultPreferences };
  let releaseHistory!: () => void, releaseRead!: () => void, getterCalls = 0;
  const historyWrite = f.characteristics[HISTORY_UUIDS.command].writeValueWithResponse;
  f.characteristics[HISTORY_UUIDS.command].writeValueWithResponse = async bytes => {
    if (bytes[0] === 1) await new Promise<void>(resolve => { releaseHistory = resolve; });
    await historyWrite(bytes);
  };
  f.profile.readValue = async () => { await new Promise<void>(resolve => { releaseRead = resolve; }); return f.profile.value; };
  const service = createLiveConnectionService({ bluetooth: f.bluetooth, getSavedPreferences: () => { getterCalls++; return saved; } });
  t.after(() => service.dispose());
  const connecting = service.connect();
  while (!releaseHistory) await new Promise(resolve => setImmediate(resolve));
  assert.equal(getterCalls, 0); assert.equal(f.profile.writes.length, 0);
  saved = { ...defaultPreferences, heartRate: 62, maxDepth: 3.2 };
  releaseHistory();
  while (!releaseRead) await new Promise(resolve => setImmediate(resolve));
  assert.equal(service.getSnapshot().history?.phase, 'live-only');
  assert.equal(service.getSnapshot().profileSync.status, 'sending');
  assert.equal(service.getSnapshot().status, 'syncing');
  assert.equal(f.characteristics[TELEMETRY_UUIDS.depth].listeners.size, 0);
  assert.deepEqual(f.profile.writes, [[...encodeProfile(saved)]]);
  releaseRead(); await connecting;
  assert.equal(service.getSnapshot().profileSync.status, 'synced');
  assert.equal(service.getSnapshot().status, 'connected');
  saved = { ...saved, heartRate: 88 };
  await service.zeroDepth();
  assert.equal(getterCalls, 1); assert.equal(f.profile.writes.length, 1);
  assert.equal(service.getSnapshot().profileSync.status, 'synced');
});

test('no saved profile skips transfer without sending defaults', async t => {
  const f = profileFixture();
  const service = createLiveConnectionService({ bluetooth: f.bluetooth, getSavedPreferences: () => null });
  t.after(() => service.dispose()); await service.connect();
  assert.equal(service.getSnapshot().status, 'connected');
  assert.equal(service.getSnapshot().profileSync.status, 'not-saved');
  assert.equal(f.profile.writes.length, 0);
});

test('settings saved during pending readback remain pending until the next connection', async t => {
  const f = profileFixture(); let saved = { ...defaultPreferences }, reads = 0;
  let releaseRead!: () => void;
  f.profile.readValue = async () => {
    const accepted = f.profile.value;
    if (++reads === 1) await new Promise<void>(resolve => { releaseRead = resolve; });
    return accepted;
  };
  const service = createLiveConnectionService({ bluetooth: f.bluetooth, getSavedPreferences: () => saved });
  t.after(() => service.dispose()); const connecting = service.connect();
  while (!releaseRead) await new Promise(resolve => setImmediate(resolve));
  saved = { ...saved, heartRate: 82, maxDepth: 3.5 };
  releaseRead(); await connecting;
  const confirmed = service.getSnapshot().profileSync;
  assert.equal(confirmed.status, 'synced');
  assert.deepEqual(confirmed.confirmedProfile, [...encodeProfile(defaultPreferences)]);
  assert.equal(Object.isFrozen(confirmed.confirmedProfile), true);
  assert.equal(profileMatchesPreferences(confirmed.confirmedProfile, saved), false, 'new save must still show pending reconnect');
  assert.equal(f.profile.writes.length, 1, 'saving must not start another transfer');
  service.disconnect(); await service.connect();
  assert.equal(profileMatchesPreferences(service.getSnapshot().profileSync.confirmedProfile, saved), true);
  assert.equal(f.profile.writes.length, 2);
});

test('firmware missing optional profile characteristic still connects with unsupported status', async t => {
  const f = fixture();
  const service = createLiveConnectionService({ bluetooth: f.bluetooth, getSavedPreferences: () => defaultPreferences });
  t.after(() => service.dispose()); await service.connect();
  assert.equal(service.getSnapshot().status, 'connected');
  assert.equal(service.getSnapshot().profileSync.status, 'unsupported');
});

test('profile discovery transport failure is an error rather than unsupported', async t => {
  const f = profileFixture(), original = f.server.getPrimaryService;
  f.server.getPrimaryService = async uuid => {
    const service = await original.call(f.server, uuid);
    return { async getCharacteristic(id) {
      if (id === PROFILE_UUID) throw new Error('Transport disconnected');
      return service.getCharacteristic(id);
    } };
  };
  const service = createLiveConnectionService({ bluetooth: f.bluetooth, getSavedPreferences: () => defaultPreferences });
  t.after(() => service.dispose()); await service.connect();
  assert.equal(service.getSnapshot().status, 'error');
  assert.equal(service.getSnapshot().profileSync.status, 'error');
  assert.match(service.getSnapshot().profileSync.message, /Transport disconnected/);
  assert.equal(f.profile.writes.length, 0);
});

test('write rejection, read failure, and rejected readback never claim a synced profile', async t => {
  for (const failure of ['write', 'read', 'mismatch', 'unconfigured'] as const) {
    const f = profileFixture();
    if (failure === 'write') f.profile.writeValueWithResponse = async () => { throw Object.assign(new Error('Write rejected'), { name: 'NotFoundError' }); };
    if (failure === 'read') f.profile.readValue = async () => { throw new Error('Read failed'); };
    if (failure === 'mismatch') f.profile.readValue = async () => new DataView(encodeProfile({ ...defaultPreferences, heartRate: 60 }).buffer);
    if (failure === 'unconfigured') f.profile.writeValueWithResponse = async bytes => { f.profile.writes.push([...bytes]); };
    const service = createLiveConnectionService({ bluetooth: f.bluetooth, getSavedPreferences: () => defaultPreferences });
    t.after(() => service.dispose()); await service.connect();
    assert.equal(service.getSnapshot().status, 'error', failure);
    assert.equal(service.getSnapshot().profileSync.status, 'error', failure);
    assert.equal(f.characteristics[TELEMETRY_UUIDS.depth].listeners.size, 0, failure);
  }
});

test('invalid saved values fail before any profile write', async t => {
  const f = profileFixture();
  const service = createLiveConnectionService({ bluetooth: f.bluetooth, getSavedPreferences: () => ({ ...defaultPreferences, maxDepth: Infinity }) });
  t.after(() => service.dispose()); await service.connect();
  assert.equal(service.getSnapshot().status, 'error');
  assert.equal(service.getSnapshot().profileSync.status, 'error');
  assert.equal(f.profile.writes.length, 0);
});

test('disconnect during profile write suppresses stale readback and reconnect sends latest saved profile', async t => {
  const f = profileFixture(); let releaseWrite!: () => void, writes = 0, reads = 0;
  let saved = { ...defaultPreferences };
  const originalWrite = f.profile.writeValueWithResponse;
  f.profile.writeValueWithResponse = async bytes => {
    await originalWrite(bytes);
    if (++writes === 1) await new Promise<void>(resolve => { releaseWrite = resolve; });
  };
  f.profile.readValue = async () => { reads++; return f.profile.value; };
  const service = createLiveConnectionService({ bluetooth: f.bluetooth, getSavedPreferences: () => saved });
  t.after(() => service.dispose()); const old = service.connect();
  while (!releaseWrite) await new Promise(resolve => setImmediate(resolve));
  service.disconnect(); assert.equal(service.getSnapshot().profileSync.status, 'idle');
  saved = { ...saved, heartRate: 81 };
  await service.connect();
  assert.equal(service.getSnapshot().profileSync.status, 'synced');
  releaseWrite(); await old;
  assert.equal(reads, 1); assert.equal(f.server.connected, true);
  assert.equal(service.getSnapshot().status, 'connected');
  assert.deepEqual(f.profile.writes, [[...encodeProfile(defaultPreferences)], [...encodeProfile(saved)]]);
});

test('late profile read after link loss cannot overwrite the next connection state', async t => {
  const f = profileFixture(); let releaseRead!: () => void, reads = 0;
  f.profile.readValue = async () => {
    const value = f.profile.value;
    if (++reads === 1) await new Promise<void>(resolve => { releaseRead = resolve; });
    return value;
  };
  const service = createLiveConnectionService({ bluetooth: f.bluetooth, getSavedPreferences: () => defaultPreferences });
  t.after(() => service.dispose()); const old = service.connect();
  while (!releaseRead) await new Promise(resolve => setImmediate(resolve));
  f.server.disconnect();
  assert.equal(service.getSnapshot().status, 'lost'); assert.equal(service.getSnapshot().profileSync.status, 'idle');
  await service.connect(); releaseRead(); await old;
  assert.equal(service.getSnapshot().status, 'connected'); assert.equal(service.getSnapshot().profileSync.status, 'synced');
  assert.equal(f.server.connected, true);
});
