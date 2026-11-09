import type { Preferences } from './preferences';

export const PROFILE_UUID = 'e81b6221-7e57-4d8e-b6f4-889c1d4e8270';
export const PROFILE_PACKET_SIZE = 12;

/** Profile v1 uses metric integers; display-unit preferences stay in the app. */
export function encodeProfile(preferences: Preferences): Uint8Array {
  const valid = (value: number, min: number, max: number) => Number.isFinite(value) && value >= min && value <= max;
  if (!valid(preferences.heartRate, 20, 140) || !Number.isInteger(preferences.heartRate)
    || !valid(preferences.maxDepth, 0.1, 10) || !valid(preferences.weightKg, 20, 150)
    || !valid(preferences.heightCm, 80, 250) || !Number.isInteger(preferences.heightCm)) {
    throw new Error('Saved profile settings are invalid. Review and save them before reconnecting.');
  }
  const bytes = new Uint8Array(PROFILE_PACKET_SIZE);
  const view = new DataView(bytes.buffer);
  view.setUint8(0, 1);
  view.setUint8(1, 1);
  view.setUint16(2, preferences.heartRate, true);
  view.setUint16(4, Math.round(preferences.maxDepth * 1000), true);
  view.setUint16(6, Math.round(preferences.weightKg * 10), true);
  view.setUint16(8, preferences.heightCm, true);
  return bytes;
}

export function verifyProfileReadback(expected: Uint8Array, actual: DataView): void {
  if (actual.byteLength !== expected.byteLength || expected.some((value, index) => value !== actual.getUint8(index))) {
    throw new Error('Wearable did not confirm the saved profile settings. Reconnect to retry.');
  }
}

export function profileMatchesPreferences(confirmedProfile: readonly number[] | undefined, preferences: Preferences | null): boolean {
  if (!confirmedProfile || !preferences) return false;
  try {
    const expected = encodeProfile(preferences);
    return confirmedProfile.length === expected.length && expected.every((value, index) => value === confirmedProfile[index]);
  } catch { return false; }
}
