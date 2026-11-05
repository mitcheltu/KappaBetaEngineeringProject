import { decodePacket, type Readings } from './telemetryProtocol';

const suffix = '-7e57-4d8e-b6f4-889c1d4e8270';
export const HISTORY_UUIDS = { identity: `e81b6230${suffix}`, status: `e81b6231${suffix}`, data: `e81b6232${suffix}`, command: `e81b6233${suffix}` } as const;
export const RECORD_BYTES = 32;
export const MAX_BLOCK_RECORDS = 64;
export const MAX_CURSOR = 0xffffffff;
export const HistoryCommand = { OPEN: 1, ACK: 2, LIVE: 3, POLL: 4, ABORT: 5 } as const;
export const HistoryStatusType = { READY: 1, BLOCK: 2, END: 3, GAP: 4, LIVE: 5, ERROR: 6 } as const;
export type HistoryIdentity = { key: string; deviceId: string; bootId: string; flags: number };
export type HistoryStatus = { type: number; token: number; first: number; end: number; detail: number; count: number; flags: number };
export type HistoryRecord = { sequence: number; kind: number; flags: number; timeUs: string; raw: ArrayBuffer };

export function copyView(value: DataView): Uint8Array {
  return new Uint8Array(value.buffer, value.byteOffset, value.byteLength).slice();
}
export function decodeIdentity(view: DataView): HistoryIdentity {
  if (view.byteLength !== 20 || view.getUint8(0) !== 1 || view.getUint8(1) !== RECORD_BYTES)
    throw new Error('Unsupported history identity. Update the wearable firmware.');
  if (view.getUint8(3) || view.getUint16(18, true)) throw new Error('Invalid history identity reserved bytes.');
  const bytes = copyView(view);
  const hex = (start: number, end: number) => [...bytes.slice(start, end)].map(v => v.toString(16).padStart(2, '0')).join('');
  const deviceId = hex(4, 10), bootId = hex(10, 18);
  return { key: `${deviceId}:${bootId}`, deviceId, bootId, flags: view.getUint8(2) };
}
export function decodeHistoryStatus(view: DataView): HistoryStatus {
  if (view.byteLength !== 20 || view.getUint8(1) !== 1) throw new Error('Unsupported history status packet.');
  const type = view.getUint8(0);
  if (type < 1 || type > 6) throw new Error('Unknown history status.');
  return { type, token: view.getUint16(2, true), first: view.getUint32(4, true), end: view.getUint32(8, true),
    detail: view.getUint32(12, true), count: view.getUint16(16, true), flags: view.getUint16(18, true) };
}
export function historyCommand(opcode: number, token: number, cursor: number): Uint8Array {
  if (!Number.isInteger(cursor) || cursor < 0 || cursor > MAX_CURSOR || !Number.isInteger(token) || token < 1 || token > 65535)
    throw new Error('Invalid history command cursor or token.');
  const bytes = new Uint8Array(8), view = new DataView(bytes.buffer);
  view.setUint8(0, opcode); view.setUint8(1, 1); view.setUint16(2, token, true); view.setUint32(4, cursor, true);
  return bytes;
}
export function crc32(bytes: Uint8Array): number {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (crc ^ 0xffffffff) >>> 0;
}
export function decodeHistoryBlock(bytes: Uint8Array, first: number): HistoryRecord[] {
  if (!bytes.length || bytes.length % RECORD_BYTES || bytes.length > MAX_BLOCK_RECORDS * RECORD_BYTES)
    throw new Error('Invalid history block length.');
  const records: HistoryRecord[] = [];
  for (let offset = 0; offset < bytes.length; offset += RECORD_BYTES) {
    const raw = bytes.slice(offset, offset + RECORD_BYTES).buffer;
    const view = new DataView(raw), sequence = view.getUint32(0, true), kind = view.getUint8(4);
    if (sequence !== first + records.length || sequence >= MAX_CURSOR) throw new Error('Nonconsecutive history record IDs.');
    if (kind < 1 || kind > 5 || view.getUint16(6, true) !== 0) throw new Error('Unsupported history record format.');
    records.push({ sequence, kind, flags: view.getUint8(5), timeUs: view.getBigUint64(8, true).toString(), raw });
  }
  return records;
}

export class HistoryBlockAssembler {
  readonly bytes: Uint8Array;
  private readonly present: Uint8Array;
  private received = 0;
  constructor(readonly status: HistoryStatus) {
    if (status.type !== HistoryStatusType.BLOCK || status.count < 1 || status.count > MAX_BLOCK_RECORDS ||
        status.first + status.count > status.end || status.first + status.count > MAX_CURSOR)
      throw new Error('Invalid history block bounds.');
    this.bytes = new Uint8Array(status.count * RECORD_BYTES);
    this.present = new Uint8Array(this.bytes.length);
  }
  matches(status: HistoryStatus) {
    return ['token', 'first', 'end', 'detail', 'count'].every(key => this.status[key as keyof HistoryStatus] === status[key as keyof HistoryStatus]);
  }
  accept(view: DataView): { complete: boolean; added: number } {
    if (view.byteLength < 9) throw new Error('Truncated history fragment.');
    if (view.getUint16(0, true) !== this.status.token || view.getUint32(4, true) !== this.status.first)
      return { complete: this.received === this.bytes.length, added: 0 };
    const offset = view.getUint16(2, true), data = new Uint8Array(view.buffer, view.byteOffset + 8, view.byteLength - 8);
    if (offset + data.length > this.bytes.length) throw new Error('History fragment exceeds its block.');
    let added = 0;
    data.forEach((value, index) => {
      const target = offset + index;
      if (this.present[target] && this.bytes[target] !== value) throw new Error('Conflicting history fragment retry.');
      if (!this.present[target]) { this.present[target] = 1; this.received++; added++; }
      this.bytes[target] = value;
    });
    const complete = this.received === this.bytes.length;
    if (complete && crc32(this.bytes) !== this.status.detail) throw new Error('History checksum mismatch. Reconnect to retry.');
    return { complete, added };
  }
}

export const CSV_COLUMNS = ['entry', 'device_id', 'boot_session', 'sequence', 'end_exclusive', 'acquisition_us', 'kind', 'flags',
  'pressure_pa', 'depth_m', 'temperature_c', 'surface_pa', 'accel_x_g', 'accel_y_g', 'accel_z_g',
  'gyro_x_dps', 'gyro_y_dps', 'gyro_z_dps', 'red', 'infrared', 'heart_rate_bpm', 'spo2_percent',
  'battery_v', 'pump_enabled', 'pump_on', 'vibration_on', 'countdown_active', 'remaining_ms',
  'cancel_pressed', 'sos_pressed', 'ppg_overflows', 'sensor_errors', 'estimated_sample_time', 'imported_at'];
export function recordCsvValues(record: HistoryRecord): Record<string, string | number | boolean | null> {
  const view = new DataView(record.raw);
  const common = { acquisition_us: record.timeUs, kind: record.kind, flags: record.flags };
  const baseline = view.getFloat32(record.kind === 5 ? 16 : 28, true);
  if (record.kind === 5) return { ...common, surface_pa: record.flags & 1 && Number.isFinite(baseline) ? baseline : null };
  const bytes = new Uint8Array(20), live = new DataView(bytes.buffer);
  live.setUint8(0, 1); live.setUint8(1, record.flags);
  bytes.set(new Uint8Array(record.raw, 16, 12), 8);
  const kind = (['depth', 'imu', 'ppg', 'system'] as const)[record.kind - 1];
  const data = decodePacket(kind, live);
  if (kind === 'depth') { const d = data as Readings['depth']; return { ...common, pressure_pa: d.pressurePa, depth_m: d.depthM, temperature_c: d.temperatureC, surface_pa: Number.isFinite(baseline) ? baseline : null }; }
  if (kind === 'imu') { const d = data as Readings['imu']; return { ...common, accel_x_g: d.accelerationG?.[0] ?? null, accel_y_g: d.accelerationG?.[1] ?? null, accel_z_g: d.accelerationG?.[2] ?? null, gyro_x_dps: d.angularVelocityDps?.[0] ?? null, gyro_y_dps: d.angularVelocityDps?.[1] ?? null, gyro_z_dps: d.angularVelocityDps?.[2] ?? null }; }
  if (kind === 'ppg') { const d = data as Readings['ppg']; return { ...common, red: d.red, infrared: d.infrared, heart_rate_bpm: d.heartRateBpm, spo2_percent: d.spo2Percent, estimated_sample_time: Boolean(record.flags & 128) }; }
  const d = data as Readings['system'];
  return { ...common, battery_v: d.batteryV, pump_enabled: d.pumpEnabled, pump_on: d.pumpOn, vibration_on: d.vibrationOn,
    countdown_active: d.countdownActive, remaining_ms: d.remainingMs, cancel_pressed: d.cancelPressed,
    sos_pressed: d.sosPressed, ppg_overflows: d.ppgOverflowCount, sensor_errors: d.sensorErrors };
}
