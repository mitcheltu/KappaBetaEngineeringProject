export type Preferences = { heartRate: number; maxDepth: number; weightKg: number; heightCm: number };

export const defaultPreferences: Preferences = { heartRate: 70, maxDepth: 2, weightKg: 70, heightCm: 170 };
export const preferenceKey = 'kappa-beta.preferences.v1';

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
      weightKg: inRange(p.weightKg, 20, 300) ? Math.round(p.weightKg * 10) / 10 : defaultPreferences.weightKg,
      heightCm: inRange(p.heightCm, 80, 250) && Number.isInteger(p.heightCm) ? p.heightCm : defaultPreferences.heightCm,
    };
  } catch { return null; }
}
