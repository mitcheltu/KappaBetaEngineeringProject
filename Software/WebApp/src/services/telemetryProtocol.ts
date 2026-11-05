// Matches Software/DrowningWearable.ino: fixed 20-byte, little-endian packets.
export const SERVICE_UUID = 'e81b6200-7e57-4d8e-b6f4-889c1d4e8270';
export const TELEMETRY_UUIDS = {
  depth: 'e81b6210-7e57-4d8e-b6f4-889c1d4e8270',
  imu: 'e81b6211-7e57-4d8e-b6f4-889c1d4e8270',
  ppg: 'e81b6212-7e57-4d8e-b6f4-889c1d4e8270',
  system: 'e81b6213-7e57-4d8e-b6f4-889c1d4e8270',
} as const;
export const ZERO_DEPTH_UUID = 'e81b6220-7e57-4d8e-b6f4-889c1d4e8270';
export const STALE_AFTER_MS = 3000;

type Header = { sequence: number; uptimeMs: number };
export type DepthReading = Header & {
  pressurePa: number | null; depthM: number | null; temperatureC: number | null;
};
export type ImuReading = Header & {
  accelerationG: [number, number, number] | null;
  angularVelocityDps: [number, number, number] | null;
};
export type PpgReading = Header & {
  red: number | null; infrared: number | null;
  heartRateBpm: number | null; spo2Percent: number | null;
};
export type SystemReading = Header & {
  batteryV: number | null; pumpEnabled: boolean; pumpOn: boolean;
  cancelPressed: boolean; sosPressed: boolean; remainingMs: number;
  vibrationOn: boolean; countdownActive: boolean;
  ppgOverflowCount: number; sensorErrors: number;
};
export type Readings = { depth: DepthReading; imu: ImuReading; ppg: PpgReading; system: SystemReading };
export type ReadingKind = keyof Readings;

export function decodePacket<K extends ReadingKind>(kind: K, packet: DataView): Readings[K] {
  if (packet.byteLength !== 20) throw new Error(`Invalid ${kind} packet length: ${packet.byteLength}; expected 20.`);
  if (packet.getUint8(0) !== 1) throw new Error(`Unsupported ${kind} protocol version: ${packet.getUint8(0)}.`);
  const flags = packet.getUint8(1);
  const header = { sequence: packet.getUint16(2, true), uptimeMs: packet.getUint32(4, true) };
  const finiteFloat = (offset: number, valid: boolean) => {
    const value = packet.getFloat32(offset, true);
    return valid && Number.isFinite(value) ? value : null;
  };
  let reading: Readings[ReadingKind];
  switch (kind) {
    case 'depth': {
      const pressure = finiteFloat(8, Boolean(flags & 1));
      reading = { ...header,
        pressurePa: pressure !== null && pressure > 0 ? pressure : null,
        depthM: finiteFloat(12, (flags & 3) === 3),
        temperatureC: finiteFloat(16, Boolean(flags & 1)),
      };
      break;
    }
    case 'imu':
      reading = { ...header,
        accelerationG: flags & 1 ? [8, 10, 12].map(offset => packet.getInt16(offset, true) / 1000) as [number, number, number] : null,
        angularVelocityDps: flags & 1 ? [14, 16, 18].map(offset => packet.getInt16(offset, true) / 10) as [number, number, number] : null,
      };
      break;
    case 'ppg': {
      const hr = packet.getUint16(16, true), spo2 = packet.getUint16(18, true);
      reading = { ...header,
        red: flags & 1 ? packet.getUint32(8, true) : null,
        infrared: flags & 1 ? packet.getUint32(12, true) : null,
        heartRateBpm: flags & 2 && hr !== 0xffff && hr > 0 ? hr / 10 : null,
        spo2Percent: flags & 4 && spo2 !== 0xffff && spo2 <= 10000 ? spo2 / 100 : null,
      };
      break;
    }
    case 'system': {
      const battery = packet.getUint16(8, true);
      reading = { ...header,
        batteryV: flags & 1 && battery !== 0xffff ? battery / 1000 : null,
        pumpEnabled: Boolean(flags & 2), pumpOn: Boolean(flags & 4),
        cancelPressed: Boolean(flags & 8), sosPressed: Boolean(flags & 16),
        vibrationOn: Boolean(flags & 32), countdownActive: Boolean(flags & 64),
        remainingMs: packet.getUint16(10, true),
        ppgOverflowCount: packet.getUint32(12, true), sensorErrors: packet.getUint32(16, true),
      };
      break;
    }
    default: throw new Error('Unknown telemetry packet.');
  }
  return reading as Readings[K];
}
