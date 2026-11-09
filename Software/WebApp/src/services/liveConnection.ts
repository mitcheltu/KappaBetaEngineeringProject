import {
  decodePacket, SERVICE_UUID, STALE_AFTER_MS, TELEMETRY_UUIDS, ZERO_DEPTH_UUID,
  type ReadingKind, type Readings,
} from './telemetryProtocol';
import { decodeIdentity, HISTORY_UUIDS } from './historyProtocol';
import { createIndexedDbHistoryStore, type HistoryStore } from './historyStore';
import { createHistoryTransfer, type HistoryProgress } from './historyTransfer';
import type { Preferences } from './preferences';
import { encodeProfile, PROFILE_UUID, verifyProfileReadback } from './profileProtocol';

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

export type LiveStatus = 'disconnected' | 'selecting' | 'connecting' | 'syncing' | 'connected' | 'lost' | 'error' | 'unsupported';
export type ProfileSyncStatus = 'idle' | 'waiting' | 'sending' | 'synced' | 'unsupported' | 'not-saved' | 'error';
export type ProfileSync = Readonly<{ status: ProfileSyncStatus; message: string; confirmedProfile?: readonly number[] }>;
type TimedReading<K extends ReadingKind> = { data: Readings[K]; receivedAt: number; stale: boolean };
export type LiveTelemetry = { [K in ReadingKind]: TimedReading<K> | null };
export type LiveSnapshot = Readonly<{
  status: LiveStatus; deviceName: string | null; message: string;
  telemetry: LiveTelemetry; zeroPending: boolean; history: HistoryProgress | null;
  profileSync: ProfileSync;
}>;
const emptyTelemetry = (): LiveTelemetry => ({ depth: null, imu: null, ppg: null, system: null });

function browserAdapter(): BluetoothAdapter | undefined {
  if (typeof navigator === 'undefined') return undefined;
  return (navigator as Navigator & { bluetooth?: BluetoothAdapter }).bluetooth;
}

export function createLiveConnectionService(options: {
  bluetooth?: BluetoothAdapter; now?: () => number; staleAfterMs?: number; checkIntervalMs?: number;
  historyStore?: HistoryStore; historyTimeoutMs?: number; historyPollIntervalMs?: number;
  getSavedPreferences?: () => Preferences | null;
} = {}) {
  const now = options.now ?? Date.now;
  const staleAfterMs = options.staleAfterMs ?? STALE_AFTER_MS;
  const listeners = new Set<() => void>();
  let snapshot: LiveSnapshot = Object.freeze({
    status: 'disconnected', deviceName: null, message: '', telemetry: emptyTelemetry(), zeroPending: false, history: null,
    profileSync: { status: 'idle' as const, message: '' },
  });
  let generation = 0;
  let device: BleDevice | undefined;
  let command: GattCharacteristic | undefined;
  let timer: ReturnType<typeof setInterval> | undefined;
  let cleanupNotifications: (() => void)[] = [];
  let disconnectListener: (() => void) | undefined;
  let disposed = false;
  const historyStore = options.historyStore ?? createIndexedDbHistoryStore();
  let transfer: ReturnType<typeof createHistoryTransfer> | undefined;
  let gattQueue: Promise<unknown> = Promise.resolve();
  function gattOperation<T>(activeGeneration: number, operation: () => Promise<T>): Promise<T> {
    const result = gattQueue.then(() => {
      if (activeGeneration !== generation || disposed) throw new Error('Bluetooth connection cancelled.');
      return operation();
    });
    gattQueue = result.catch(() => {});
    return result;
  }
  function disconnectAbandoned(server: GattServer, previous: BleDevice) {
    // Reconnecting can reuse the same device/server object. A late completion
    // from an older attempt must not disconnect the newer connection.
    if (device !== previous) server.disconnect();
  }

  function publish(patch: Partial<LiveSnapshot>) {
    if (disposed) return;
    snapshot = Object.freeze({ ...snapshot, ...patch });
    for (const listener of [...listeners]) listener();
  }

  function release() {
    transfer?.cancel(); transfer = undefined;
    gattQueue = Promise.resolve();
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
    publish({ status: 'disconnected', deviceName: null, telemetry: emptyTelemetry(), message: '', zeroPending: false,
      profileSync: { status: 'idle', message: '' } });
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
    if (disposed || ['selecting', 'connecting', 'syncing', 'connected'].includes(snapshot.status)) return;
    const bluetooth = options.bluetooth ?? browserAdapter();
    if (!bluetooth) {
      publish({ status: 'unsupported', message: 'Web Bluetooth is unavailable. Open this page over HTTPS or localhost in Chrome on Windows, macOS, or Android.' });
      return;
    }
    const activeGeneration = ++generation;
    release();
    publish({ status: 'selecting', telemetry: emptyTelemetry(), message: '', deviceName: null, history: null,
      profileSync: { status: 'waiting', message: 'Saved settings will transfer after history finishes.' } });
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
        publish({ status: 'lost', telemetry: emptyTelemetry(), zeroPending: false, message: 'Device disconnected. Reconnect to resume readings.',
          profileSync: { status: 'idle', message: 'Disconnected. Saved settings will transfer on the next connection.' } });
      };
      chosen.addEventListener('gattserverdisconnected', disconnectListener);
      publish({ status: 'connecting', deviceName: chosen.name || 'KappaBetaWearable' });
      const server = await chosen.gatt.connect();
      if (activeGeneration !== generation || disposed) { disconnectAbandoned(server, chosen); return; }
      const service = await server.getPrimaryService(SERVICE_UUID);
      if (activeGeneration !== generation || disposed) { disconnectAbandoned(server, chosen); return; }
      let historyCharacteristics: Record<keyof typeof HISTORY_UUIDS, GattCharacteristic>;
      try {
        historyCharacteristics = {
          identity: await service.getCharacteristic(HISTORY_UUIDS.identity),
          status: await service.getCharacteristic(HISTORY_UUIDS.status),
          data: await service.getCharacteristic(HISTORY_UUIDS.data),
          command: await service.getCharacteristic(HISTORY_UUIDS.command),
        };
      } catch {
        throw new Error('Firmware update required: this wearable does not provide the sensor history service.');
      }
      if (activeGeneration !== generation || disposed) { disconnectAbandoned(server, chosen); return; }
      const identity = decodeIdentity(await historyCharacteristics.identity.readValue());
      if (activeGeneration !== generation || disposed) { disconnectAbandoned(server, chosen); return; }
      const currentTransfer = createHistoryTransfer({ identity, store: historyStore,
        write: bytes => gattOperation(activeGeneration, () => historyCharacteristics.command.writeValueWithResponse(bytes)),
        readStatus: () => gattOperation(activeGeneration, () => historyCharacteristics.status.readValue()),
        now, timeoutMs: options.historyTimeoutMs, pollIntervalMs: options.historyPollIntervalMs,
        onProgress: history => { if (activeGeneration === generation) publish({ history }); },
        onError: error => {
          if (activeGeneration !== generation) return;
          generation += 1; release();
          publish({ status: 'error', telemetry: emptyTelemetry(), zeroPending: false, message: error.message });
        },
      });
      transfer = currentTransfer;
      for (const kind of ['status', 'data'] as const) {
        const characteristic = historyCharacteristics[kind];
        const notify = () => {
          if (activeGeneration !== generation || !characteristic.value) return;
          if (kind === 'status') currentTransfer.handleStatus(characteristic.value);
          else currentTransfer.handleData(characteristic.value);
        };
        characteristic.addEventListener('characteristicvaluechanged', notify);
        cleanupNotifications.push(() => characteristic.removeEventListener('characteristicvaluechanged', notify));
        await characteristic.startNotifications();
        if (activeGeneration !== generation || disposed) { disconnectAbandoned(server, chosen); return; }
      }
      publish({ status: 'syncing' });
      await currentTransfer.start();
      if (activeGeneration !== generation || disposed) { disconnectAbandoned(server, chosen); return; }
      await syncSavedProfile(activeGeneration, uuid => service.getCharacteristic(uuid));
      if (activeGeneration !== generation || disposed) { disconnectAbandoned(server, chosen); return; }
      for (const kind of Object.keys(TELEMETRY_UUIDS) as ReadingKind[]) {
        const characteristic = await gattOperation(activeGeneration, () => service.getCharacteristic(TELEMETRY_UUIDS[kind]));
        if (activeGeneration !== generation || disposed) { disconnectAbandoned(server, chosen); return; }
        const notify = () => { if (characteristic.value) receive(kind, characteristic.value, activeGeneration); };
        characteristic.addEventListener('characteristicvaluechanged', notify);
        cleanupNotifications.push(() => characteristic.removeEventListener('characteristicvaluechanged', notify));
        await gattOperation(activeGeneration, () => characteristic.startNotifications());
        if (activeGeneration !== generation || disposed) { disconnectAbandoned(server, chosen); return; }
        receive(kind, await gattOperation(activeGeneration, () => characteristic.readValue()), activeGeneration);
      }
      const zero = await gattOperation(activeGeneration, () => service.getCharacteristic(ZERO_DEPTH_UUID));
      if (activeGeneration !== generation || disposed) { disconnectAbandoned(server, chosen); return; }
      command = zero;
      publish({ status: 'connected' });
      timer = setInterval(refreshStaleness, options.checkIntervalMs ?? 500);
      currentTransfer.enablePolling();
    } catch (error) {
      if (activeGeneration !== generation || disposed) { if (chosen?.gatt) disconnectAbandoned(chosen.gatt, chosen); return; }
      generation += 1;
      release();
      const cancelled = !chosen && error instanceof Error && error.name === 'NotFoundError';
      publish({ status: cancelled ? 'disconnected' : 'error', telemetry: emptyTelemetry(),
        ...(snapshot.profileSync.status === 'waiting' ? { profileSync: { status: 'idle' as const, message: '' } } : {}),
        message: cancelled ? 'Device selection cancelled.' : error instanceof Error ? error.message : 'Bluetooth connection failed.' });
    }
  }

  async function syncSavedProfile(activeGeneration: number, getCharacteristic: (uuid: string) => Promise<GattCharacteristic>) {
    try {
      // Read at transfer time, after history: never capture drafts or stale settings at service creation.
      const preferences = options.getSavedPreferences?.() ?? null;
      if (!preferences) {
        publish({ profileSync: { status: 'not-saved', message: 'No saved settings to send. Save your settings, then reconnect.' } });
        return;
      }
      const bytes = encodeProfile(preferences);
      let characteristic: GattCharacteristic;
      try {
        characteristic = await gattOperation(activeGeneration, () => getCharacteristic(PROFILE_UUID));
      } catch (error) {
        if (activeGeneration !== generation || disposed) return;
        if (error instanceof Error && error.name === 'NotFoundError') {
          publish({ profileSync: { status: 'unsupported', message: 'This firmware does not support profile settings. Your settings remain saved in this browser.' } });
          return;
        }
        throw error;
      }
      if (activeGeneration !== generation || disposed) return;
      publish({ profileSync: { status: 'sending', message: 'Sending saved settings and checking device receipt…' } });
      await gattOperation(activeGeneration, () => characteristic.writeValueWithResponse(bytes));
      if (activeGeneration !== generation || disposed) return;
      const accepted = await gattOperation(activeGeneration, () => characteristic.readValue());
      if (activeGeneration !== generation || disposed) return;
      verifyProfileReadback(bytes, accepted);
      publish({ profileSync: { status: 'synced', confirmedProfile: Object.freeze([...bytes]),
        message: 'Profile received. Settings do not control triggers yet.' } });
    } catch (error) {
      if (activeGeneration !== generation || disposed) return;
      const message = error instanceof Error ? error.message : 'Profile settings transfer failed. Reconnect to retry.';
      publish({ profileSync: { status: 'error', message } });
      throw new Error(`Profile settings transfer failed: ${message}`);
    }
  }

  async function zeroDepth() {
    if (snapshot.status !== 'connected' || snapshot.zeroPending || !command) return;
    const activeGeneration = generation;
    const depth = snapshot.telemetry.depth;
    publish({ zeroPending: true, message: 'Requesting a surface pressure baseline…',
      telemetry: { ...snapshot.telemetry, depth: depth ? { ...depth, data: { ...depth.data, depthM: null } } : null } });
    try {
      const currentCommand = command;
      await gattOperation(activeGeneration, () => currentCommand.writeValueWithResponse(new Uint8Array([1])));
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
