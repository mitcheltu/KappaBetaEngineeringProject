// Development-only fixtures make the specification's stress screenshots reproducible.
// These values have no effect in a production export.
const params = __DEV__ ? new URLSearchParams(window.location.search) : undefined;
export const auditDeviceName = params?.get('auditName') === 'long'
  ? 'Demo Wearable ESP32 — Kappa Beta Beach Research Prototype 001'
  : undefined;

export function applyAuditZoom() {
  if (__DEV__ && params?.get('auditZoom') === '2') {
    document.documentElement.style.zoom = '2';
  }
}
