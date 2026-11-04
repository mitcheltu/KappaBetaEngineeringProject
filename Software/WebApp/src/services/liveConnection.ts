import {
  decodePacket, SERVICE_UUID, STALE_AFTER_MS, TELEMETRY_UUIDS, ZERO_DEPTH_UUID,
  type ReadingKind, type Readings,
} from './telemetryProtocol';

// Narrow Web Bluetooth interfaces avoid a runtime dependency and allow transport tests.
export interface GattCharacteristic {
  value?: DataView;
  startNotifications(): Promise<unknown>;
  readValue(): Promise<DataView>;
  writeValueWithResponse(value: Uint8Array): Promise<void>;
  addEventListener(type: string, callback: () => void): void;
  removeEventListener(type: string, callback: () => void): void;
}
export interface GattServer {
  connected: boolean;
  connect(): Promise<GattServer>;
  disconnect(): void;
  getPrimaryService(uuid: string): Promise<{ getCharacteristic(uuid: string): Promise<GattCharacteristic> }>;
}
export interface BleDevice {
  name?: string;
  gatt?: GattServer;
  addEventListener(type: string, callback: () => void): void;
  removeEventListener(type: string, callback: () => void): void;
}
export interface BluetoothAdapter {
  requestDevice(options: { filters: { services: string[] }[] }): Promise<BleDevice>;
}

export type LiveStatus = 'disconnected' | 'selecting' | 'connecting' | 'connected' | 'lost' | 'error' | 'unsupported';
type TimedReading<K extends ReadingKind> = { data: Readings[K]; receivedAt: number; stale: boolean };
export type LiveTelemetry = { [K in ReadingKind]: TimedReading<K> | null };
export type LiveSnapshot = Readonly<{
  status: LiveStatus; deviceName: string | null; message: string;
  telemetry: LiveTelemetry; zeroPending: boolean;
}>;
const emptyTelemetry = (): LiveTelemetry => ({ depth: null, imu: null, ppg: null, system: null });

function browserAdapter(): BluetoothAdapter | undefined {
  if (typeof navigator === 'undefined') return undefined;
  return (navigator as Navigator & { bluetooth?: BluetoothAdapter }).bluetooth;
}

export function createLiveConnectionService(options: {
  bluetooth?: BluetoothAdapter; now?: () => number; staleAfterMs?: number; checkIntervalMs?: number;
} = {}) {
  const now = options.now ?? Date.now;
  const staleAfterMs = options.staleAfterMs ?? STALE_AFTER_MS;
  const listeners = new Set<() => void>();
  let snapshot: LiveSnapshot = Object.freeze({
    status: 'disconnected', deviceName: null, message: '', telemetry: emptyTelemetry(), zeroPending: false,
  });
  let generation = 0;
  let device: BleDevice | undefined;
  let command: GattCharacteristic | undefined;
  let timer: ReturnType<typeof setInterval> | undefined;
  let cleanupNotifications: (() => void)[] = [];
  let disconnectListener: (() => void) | undefined;
  let disposed = false;

  function publish(patch: Partial<LiveSnapshot>) {
    if (disposed) return;
    snapshot = Object.freeze({ ...snapshot, ...patch });
    for (const listener of [...listeners]) listener();
  }

  function release() {
    if (timer !== undefined) clearInterval(timer);
    timer = undefined;
    for (const cleanup of cleanupNotifications) cleanup();
    cleanupNotifications = [];
    if (device && disconnectListener) device.removeEventListener('gattserverdisconnected', disconnectListener);
    disconnectListener = undefined;
    const previous = device;
    device = undefined;
    command = undefined;
    previous?.gatt?.disconnect();
  }

  function disconnect() {
    generation += 1;
    release();
    publish({ status: 'disconnected', deviceName: null, telemetry: emptyTelemetry(), message: '', zeroPending: false });
  }

  function refreshStaleness() {
    if (snapshot.status !== 'connected') return;
    let changed = false;
    const telemetry = { ...snapshot.telemetry };
    for (const kind of Object.keys(TELEMETRY_UUIDS) as ReadingKind[]) {
      const current = telemetry[kind];
      if (current && !current.stale && now() - current.receivedAt >= staleAfterMs) {
        // Each union member retains its own matching key.
        (telemetry as Record<ReadingKind, unknown>)[kind] = { ...current, stale: true };
        changed = true;
      }
    }
    if (changed) publish({ telemetry });
  }

  function receive<K extends ReadingKind>(kind: K, value: DataView, activeGeneration: number) {
    if (activeGeneration !== generation || disposed) return;
    try {
      const data = decodePacket(kind, value);
      const previous = snapshot.telemetry[kind]?.data;
      // Ignore duplicate reads and late reads that race an incoming notification.
      if (previous) {
        const elapsed = (data.uptimeMs - previous.uptimeMs) >>> 0;
        if ((elapsed === 0 && data.sequence === previous.sequence) || elapsed >= 0x80000000) return;
      }
      publish({ telemetry: { ...snapshot.telemetry, [kind]: { data, receivedAt: now(), stale: false } } });
    } catch (error) {
      publish({ telemetry: { ...snapshot.telemetry, [kind]: null }, message: error instanceof Error ? error.message : 'Invalid sensor packet.' });
    }
  }

  async function connect() {
    if (disposed || ['selecting', 'connecting', 'connected'].includes(snapshot.status)) return;
    const bluetooth = options.bluetooth ?? browserAdapter();
    if (!bluetooth) {
      publish({ status: 'unsupported', message: 'Web Bluetooth is unavailable. Open this page over HTTPS or localhost in Chrome on Windows, macOS, or Android.' });
      return;
    }
    const activeGeneration = ++generation;
    release();
    publish({ status: 'selecting', telemetry: emptyTelemetry(), message: '', deviceName: null });
    let chosen: BleDevice | undefined;
    try {
      // Do not add an await before this call: requestDevice requires the button gesture.
      chosen = await bluetooth.requestDevice({ filters: [{ services: [SERVICE_UUID] }] });
      if (activeGeneration !== generation || disposed) return;
      device = chosen;
      if (!chosen.gatt) throw new Error('This device has no Bluetooth GATT server.');
      disconnectListener = () => {
        if (activeGeneration !== generation) return;
        generation += 1;
        release();
        publish({ status: 'lost', telemetry: emptyTelemetry(), zeroPending: false, message: 'Device disconnected. Reconnect to resume readings.' });
      };
      chosen.addEventListener('gattserverdisconnected', disconnectListener);
      publish({ status: 'connecting', deviceName: chosen.name || 'KappaBeta-RevF' });
      const server = await chosen.gatt.connect();
      if (activeGeneration !== generation || disposed) { server.disconnect(); return; }
      const service = await server.getPrimaryService(SERVICE_UUID);
      for (const kind of Object.keys(TELEMETRY_UUIDS) as ReadingKind[]) {
        const characteristic = await service.getCharacteristic(TELEMETRY_UUIDS[kind]);
        if (activeGeneration !== generation || disposed) { server.disconnect(); return; }
        const notify = () => { if (characteristic.value) receive(kind, characteristic.value, activeGeneration); };
        characteristic.addEventListener('characteristicvaluechanged', notify);
        cleanupNotifications.push(() => characteristic.removeEventListener('characteristicvaluechanged', notify));
        await characteristic.startNotifications();
        if (activeGeneration !== generation || disposed) { server.disconnect(); return; }
        receive(kind, await characteristic.readValue(), activeGeneration);
      }
      const zero = await service.getCharacteristic(ZERO_DEPTH_UUID);
      if (activeGeneration !== generation || disposed) { server.disconnect(); return; }
      command = zero;
      publish({ status: 'connected' });
      timer = setInterval(refreshStaleness, options.checkIntervalMs ?? 500);
    } catch (error) {
      if (activeGeneration !== generation || disposed) { chosen?.gatt?.disconnect(); return; }
      release();
      const cancelled = error instanceof Error && error.name === 'NotFoundError';
      publish({ status: cancelled ? 'disconnected' : 'error', telemetry: emptyTelemetry(),
        message: cancelled ? 'Device selection cancelled.' : error instanceof Error ? error.message : 'Bluetooth connection failed.' });
    }
  }

  async function zeroDepth() {
    if (snapshot.status !== 'connected' || snapshot.zeroPending || !command) return;
    const activeGeneration = generation;
    const depth = snapshot.telemetry.depth;
    publish({ zeroPending: true, message: 'Requesting a surface pressure baseline…',
      telemetry: { ...snapshot.telemetry, depth: depth ? { ...depth, data: { ...depth.data, depthM: null } } : null } });
    try {
      await command.writeValueWithResponse(new Uint8Array([1]));
      if (activeGeneration === generation) publish({ zeroPending: false, message: 'Surface zero requested; allow the next readings to settle. Check Serial for the calibration result.' });
    } catch (error) {
      if (activeGeneration === generation) publish({ zeroPending: false, message: error instanceof Error ? error.message : 'Surface zero failed.' });
    }
  }

  return {
    getSnapshot: () => snapshot,
    subscribe(listener: () => void) { listeners.add(listener); return () => { listeners.delete(listener); }; },
    connect, disconnect, zeroDepth, refreshStaleness,
    dispose() { disconnect(); disposed = true; listeners.clear(); },
  };
}
