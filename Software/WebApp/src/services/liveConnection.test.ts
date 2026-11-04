import assert from 'node:assert/strict';
import test from 'node:test';
import { createLiveConnectionService, type BleDevice, type BluetoothAdapter, type GattCharacteristic, type GattServer } from './liveConnection';
import { decodePacket, SERVICE_UUID, TELEMETRY_UUIDS, ZERO_DEPTH_UUID } from './telemetryProtocol';

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
    sequence: 12, uptimeMs: 1234, batteryV: 4.1, valveEnabled: true, valveOn: true,
    cancelPressed: true, sosPressed: true, remainingMs: 250, ppgOverflowCount: 42, sensorErrors: 123,
    vibrationOn: false, countdownActive: false,
  });
  system.setUint16(8, 0xffff, true); assert.equal(decodePacket('system', system).batteryV, null);
});

test('system flags distinguish a vibrating countdown from a valve held open', () => {
  const system = packet(1 | 2 | 32 | 64);
  system.setUint16(10, 10000, true);
  const countdown = decodePacket('system', system);
  assert.equal(countdown.vibrationOn, true);
  assert.equal(countdown.countdownActive, true);
  assert.equal(countdown.valveOn, false);
  assert.equal(countdown.remainingMs, 10000);
  system.setUint8(1, 1 | 2 | 4);
  system.setUint16(10, 0, true);
  const open = decodePacket('system', system);
  assert.equal(open.vibrationOn, false);
  assert.equal(open.countdownActive, false);
  assert.equal(open.valveOn, true);
  assert.equal(open.remainingMs, 0);
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
  const characteristics = Object.fromEntries([...Object.values(TELEMETRY_UUIDS), ZERO_DEPTH_UUID].map(uuid => [uuid, new Characteristic()]));
  const device: BleDevice & Events = Object.assign(new Events(), { name: 'KappaBeta-RevF', gatt: undefined as GattServer | undefined });
  const server: GattServer = {
    connected: false,
    async connect() { this.connected = true; return this; },
    disconnect() { const wasConnected = this.connected; this.connected = false; if (wasConnected) device.emit('gattserverdisconnected'); },
    async getPrimaryService(uuid) {
      assert.equal(uuid, SERVICE_UUID);
      return { async getCharacteristic(characteristicUuid: string) { return characteristics[characteristicUuid]; } };
    },
  };
  device.gatt = server;
  const bluetooth: BluetoothAdapter = { async requestDevice(options) {
    assert.deepEqual(options, { filters: [{ services: [SERVICE_UUID] }] }); return device;
  } };
  return { bluetooth, device, server, characteristics };
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
