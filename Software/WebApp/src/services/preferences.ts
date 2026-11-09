export type WeightUnit = 'kg' | 'lbs';
export type Preferences = { heartRate: number; maxDepth: number; weightKg: number; weightUnit: WeightUnit; heightCm: number };

export const defaultPreferences: Preferences = { heartRate: 70, maxDepth: 2, weightKg: 70, weightUnit: 'kg', heightCm: 170 };
export const preferenceKey = 'kappa-beta.preferences.v1';

const kgPerPound = 0.45359237;
export function displayWeight(weightKg: number, unit: WeightUnit): number {
  return Math.round((unit === 'lbs' ? weightKg / kgPerPound : weightKg) * 10) / 10;
}
export function weightToKg(value: number, unit: WeightUnit): number {
  if (value <= displayWeight(20, unit)) return 20;
  if (value >= displayWeight(150, unit)) return 150;
  return unit === 'lbs' ? value * kgPerPound : value;
}

function inRange(value: unknown, min: number, max: number): value is number {
  return typeof value === 'number' && Number.isFinite(value) && value >= min && value <= max;
}

/** Keep existing baselines when upgrading preferences saved before body measurements existed. */
export function parsePreferences(raw: string | null): Preferences | null {
  try {
    const value: unknown = JSON.parse(raw || 'null');
    if (!value || typeof value !== 'object') return null;
    const p = value as Record<string, unknown>;
    if (!inRange(p.heartRate, 20, 140) || !Number.isInteger(p.heartRate) || !inRange(p.maxDepth, 0.1, 10)) return null;
    return {
      heartRate: p.heartRate,
      maxDepth: p.maxDepth,
      // Clamp weights saved under the previous 300 kg limit to the new maximum.
      weightKg: inRange(p.weightKg, 20, 300) ? Math.min(p.weightKg, 150) : defaultPreferences.weightKg,
      weightUnit: p.weightUnit === 'lbs' ? 'lbs' : 'kg',
      heightCm: inRange(p.heightCm, 80, 250) && Number.isInteger(p.heightCm) ? p.heightCm : defaultPreferences.heightCm,
    };
  } catch { return null; }
}
