import { useEffect, useState, useSyncExternalStore } from 'react';
import { Platform, Pressable, StyleSheet, Text, View } from 'react-native';
import { createLiveConnectionService } from '../services/liveConnection';
import { createIndexedDbHistoryStore } from '../services/historyStore';
import { SavedHistoryPanel } from './SavedHistoryPanel';
import { colors } from '../theme';
import { ActionButton } from './ActionButton';

const mono = Platform.OS === 'web' ? 'Space Mono, monospace' : 'monospace';
const display = Platform.OS === 'web' ? 'Barlow Condensed, sans-serif' : undefined;
const format = (value: number | null | undefined, digits: number, unit: string) =>
  value == null ? '—' : `${value.toFixed(digits)}${unit ? ` ${unit}` : ''}`;
const vector = (values: [number, number, number] | null | undefined, digits: number) =>
  values ? values.map(value => value.toFixed(digits)).join(' / ') : '—';

export function LiveTelemetryPanel() {
  const [historyStore] = useState(() => createIndexedDbHistoryStore());
  const [service] = useState(() => createLiveConnectionService({ historyStore }));
  const snapshot = useSyncExternalStore(service.subscribe, service.getSnapshot, service.getSnapshot);
  useEffect(() => () => service.disconnect(), [service]);
  const connected = snapshot.status === 'connected';
  const pending = snapshot.status === 'selecting' || snapshot.status === 'connecting' || snapshot.status === 'syncing';
  const { depth, imu, ppg, system } = snapshot.telemetry;
  const d = depth && !depth.stale ? depth.data : null;
  const i = imu && !imu.stale ? imu.data : null;
  const p = ppg && !ppg.stale ? ppg.data : null;
  const s = system && !system.stale ? system.data : null;
  const actuatorStatus = !s ? 'Waiting for device status'
    : s.countdownActive ? `Countdown ${Math.ceil(s.remainingMs / 1000)} s / ${s.vibrationOn ? 'blue LED on' : 'blue LED off'}`
      : s.pumpOn ? 'Pump running until Cancel'
        : s.pumpEnabled ? 'Idle · pump stopped' : 'Pump disabled';
  const hasStale = Object.values(snapshot.telemetry).some(reading => reading?.stale);
  const linkLabel = connected ? (hasStale ? 'CONNECTED / DATA STALE' : snapshot.history?.phase === 'live-only' ? 'CONNECTED / LIVE ONLY' : 'CONNECTED / LIVE')
    : snapshot.status === 'syncing' ? 'SAVING HISTORY BEFORE LIVE' : pending ? 'CONNECTING' : snapshot.status === 'lost' ? 'LINK LOST' : 'OFFLINE';

  return <View nativeID="live-telemetry" style={styles.panel}>
    <View style={styles.header}>
      <View style={styles.identity}>
        <Text style={styles.eyebrow}>ESP32 / LIVE SENSOR READINGS</Text>
        <Text accessibilityRole="header" style={styles.heading}>{snapshot.deviceName || 'CONNECT YOUR WEARABLE'}</Text>
        <Text accessibilityLiveRegion="polite" style={styles.status}>{linkLabel}</Text>
      </View>
      <ActionButton label={connected ? 'Disconnect wearable' : pending ? 'Cancel connection' : 'Connect wearable'}
        icon={connected || pending ? undefined : 'bluetooth'} secondary={connected || pending}
        onPress={connected || pending ? service.disconnect : () => { void service.connect(); }} />
    </View>
    <Text style={styles.description}>Select KappaBeta-RevF in the browser’s Bluetooth picker. Saved sensor history downloads first, then live readings begin. The controls above are a separate demo.</Text>
    <SavedHistoryPanel store={historyStore} progress={snapshot.history} connected={connected || snapshot.status === 'syncing'} />
    <View style={styles.grid}>
      <Reading label="DEPTH" value={format(d?.depthM, 3, 'm')} note={depth?.stale ? 'Stale reading' : d?.pressurePa != null && d.depthM == null ? 'Zero at the surface first' : 'Relative to surface baseline'} />
      <Reading label="PRESSURE" value={format(d?.pressurePa, 0, 'Pa')} note={`Temperature ${format(d?.temperatureC, 2, '°C')}`} />
      <Reading label="HEART RATE / PPG" value={format(p?.heartRateBpm, 0, 'bpm')} note={ppg?.stale ? 'Stale reading' : 'Requires a valid optical signal'} />
      <Reading label="SpO₂ / PPG" value={format(p?.spo2Percent, 0, '%')} note="Experimental sensor estimate" />
      <Reading label="ACCELERATION X / Y / Z" value={vector(i?.accelerationG, 3)} note={imu?.stale ? 'Stale reading' : 'g · includes gravity'} compact />
      <Reading label="GYROSCOPE X / Y / Z" value={vector(i?.angularVelocityDps, 1)} note="Degrees per second" compact />
      <Reading label="OPTICAL RAW / RED / IR" value={p?.red == null || p.infrared == null ? '—' : `${p.red} / ${p.infrared}`} note="Raw sensor counts" compact />
      <Reading label="BATTERY" value={format(s?.batteryV, 3, 'V')} note={actuatorStatus} />
    </View>
    <View style={styles.calibration}>
      <View style={styles.calibrationCopy}>
        <Text style={styles.label}>SURFACE BASELINE</Text>
        <Text style={styles.description}>Keep the pressure sensor at the water surface or in air before zeroing. Missing or stale measurements display as —.</Text>
      </View>
      <Pressable accessibilityRole="button" accessibilityState={{ disabled: !connected || snapshot.zeroPending }}
        disabled={!connected || snapshot.zeroPending} onPress={() => { void service.zeroDepth(); }}
        style={[styles.zeroButton, (!connected || snapshot.zeroPending) && styles.disabled]}>
        <Text style={styles.zeroText}>{snapshot.zeroPending ? 'ZEROING…' : 'ZERO DEPTH AT SURFACE'}</Text>
      </Pressable>
    </View>
    {!!snapshot.message && <Text accessibilityLiveRegion="polite" style={styles.message}>{snapshot.message}</Text>}
    {s && <View style={{ gap: 6 }}>
      <Text style={styles.diagnostics}>Blue LED {s.vibrationOn ? 'ON' : 'OFF'} · PPG overflows {s.ppgOverflowCount} · Sensor errors {s.sensorErrors} · Cancel {s.cancelPressed ? 'pressed' : 'released'} · SOS {s.sosPressed ? 'pressed' : 'released'}</Text>
      <Text style={styles.footnote}>Pump and blue LED status show commanded outputs. Water flow is not measured. Use the physical Cancel button to abort the test or stop the pump.</Text>
    </View>}
    <Text style={styles.footnote}>Use Chrome on Windows, macOS, or Android over HTTPS or localhost. No drowning detection or remote pump activation.</Text>
  </View>;
}

function Reading({ label, value, note, compact = false }: { label: string; value: string; note: string; compact?: boolean }) {
  return <View style={styles.reading}><Text style={styles.label}>{label}</Text><Text style={[styles.value, compact && styles.compact]}>{value}</Text><Text style={styles.note}>{note}</Text></View>;
}

const styles = StyleSheet.create({
  panel: { marginTop: 28, padding: 24, borderWidth: 1, borderColor: colors.line, borderTopWidth: 3, borderTopColor: colors.sea, backgroundColor: colors.card, gap: 18 },
  header: { flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', justifyContent: 'space-between', gap: 18 },
  identity: { flex: 1, minWidth: 200, gap: 5 },
  eyebrow: { fontFamily: mono, fontSize: 10, lineHeight: 18, color: colors.sea },
  heading: { fontFamily: display, fontWeight: '700', fontSize: 30, lineHeight: 36, color: colors.ink },
  status: { fontFamily: mono, fontSize: 10, lineHeight: 18, color: colors.sea },
  description: { fontSize: 13, lineHeight: 21, color: colors.muted },
  grid: { flexDirection: 'row', flexWrap: 'wrap', borderTopWidth: 1, borderColor: colors.line },
  reading: { flexGrow: 1, flexBasis: 240, minWidth: 0, paddingVertical: 16, paddingRight: 16, gap: 6, borderBottomWidth: 1, borderColor: colors.line },
  label: { fontFamily: mono, fontSize: 10, lineHeight: 18, color: colors.muted },
  value: { fontFamily: mono, fontSize: 24, lineHeight: 34, color: colors.ink },
  compact: { fontSize: 16, lineHeight: 26 },
  note: { fontSize: 12, lineHeight: 18, color: colors.muted },
  calibration: { flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: 20 },
  calibrationCopy: { flexGrow: 1, flexBasis: 280, minWidth: 0, gap: 5 },
  zeroButton: { minHeight: 48, justifyContent: 'center', borderWidth: 1, borderColor: colors.sea, padding: 12 },
  zeroText: { fontFamily: mono, fontSize: 10, lineHeight: 18, color: colors.sea },
  disabled: { opacity: 0.4 },
  message: { fontSize: 13, lineHeight: 21, color: colors.sea },
  diagnostics: { fontFamily: mono, fontSize: 10, lineHeight: 20, color: colors.muted },
  footnote: { fontSize: 12, lineHeight: 20, color: colors.muted },
});
