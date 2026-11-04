import { useEffect, useRef, useState, useSyncExternalStore, type ComponentRef } from 'react';
import { Image, Platform, Pressable, ScrollView, StyleSheet, Text, View, useWindowDimensions } from 'react-native';
import { colors } from './theme';
import { auditDeviceName } from './auditFixture';
import { createMockConnectionService, type Scenario } from './services/mockConnection';
import { ActionButton } from './components/ActionButton';
import { DemoValueSlider } from './components/DemoValueSlider';
import { DevicePickerSheet } from './components/DevicePickerSheet';
import { Icon } from './components/Icon';
import { LiveTelemetryPanel } from './components/LiveTelemetryPanel';

const displayFont = Platform.OS === 'web' ? 'Barlow Condensed, sans-serif' : undefined;
const monoFont = Platform.OS === 'web' ? 'Space Mono, monospace' : Platform.OS === 'ios' ? 'Menlo' : 'monospace';
const scenarios: { value: Scenario; label: string; accessibleLabel: string }[] = [
  { value: 'normal', label: 'NORMAL', accessibleLabel: 'Normal discovery' },
  { value: 'empty', label: 'EMPTY', accessibleLabel: 'No devices found' },
  { value: 'failure', label: 'FAIL', accessibleLabel: 'Connection failure' },
];
const depthMarks = [0, 2, 4, 6, 8, 10];
const selectedButtonProps = (selected: boolean) => Platform.OS === 'web'
  ? { 'aria-pressed': selected }
  : { accessibilityState: { selected } };

export default function App() {
  const [service] = useState(() => createMockConnectionService({ deviceName: auditDeviceName }));
  const snapshot = useSyncExternalStore(service.subscribe, service.getSnapshot, service.getSnapshot);
  const [heartRate, setHeartRate] = useState(70);
  const [depth, setDepth] = useState(0);
  const [resetMessage, setResetMessage] = useState('');
  const connectionActionRef = useRef<ComponentRef<typeof View>>(null);
  const { width } = useWindowDimensions();
  const connected = snapshot.status === 'connected';
  const lost = snapshot.status === 'lost';
  useEffect(() => { if (lost) connectionActionRef.current?.focus(); }, [lost]);
  useEffect(() => () => service.close(), [service]);
  useEffect(() => {
    if (!resetMessage) return;
    const timer = setTimeout(() => setResetMessage(''), 2200);
    return () => clearTimeout(timer);
  }, [resetMessage]);
  const Page = Platform.OS === 'web' ? View : ScrollView;

  return <Page style={styles.page} nativeID="app-page">
    <View style={styles.container} nativeID="workspace">
      <View style={styles.header} nativeID="app-header">
        <View style={styles.wordmark}><Icon name="wave" size={32} color={colors.ink} /><Text accessibilityRole="header" nativeID="wordmark" style={styles.brand}>KAPPA BETA</Text></View>
        <View style={styles.headerMeta}><View style={styles.demoBadge}><Text style={styles.demoText}>BENCH TELEMETRY + DEMO</Text></View></View>
      </View>

      <View nativeID="dashboard-grid" style={[styles.dashboard, width >= 960 && styles.dashboardWide]}>
        <View nativeID="field-column" style={styles.fieldColumn}>
          <View nativeID="depth-stage" style={styles.stage}>
            <Image source={require('../assets/ocean-background.png')} resizeMode="cover" accessible={false} pointerEvents="none" style={styles.stageImage} />
            <View nativeID="stage-shade" pointerEvents="none" style={styles.stageShade} />
            <View style={styles.stageHeader}><Text style={styles.stageLabel}>DEPTH SIMULATOR</Text><Text style={styles.stageLabel}>METERS ↓</Text></View>
            <View nativeID="depth-field" style={styles.depthField}>
              {depthMarks.map(mark => <View key={mark} pointerEvents="box-none" style={[styles.depthMarkRow, { top: `${mark * 10}%` }]}>
                <Pressable accessibilityRole="button" accessibilityLabel={`Set estimated depth to ${mark} meters`} {...selectedButtonProps(depth === mark)} onPress={() => setDepth(mark)} style={({ pressed }) => [styles.depthMark, depth === mark && styles.depthMarkSelected, pressed && { opacity: 0.75 }]}><Text style={[styles.depthMarkText, depth === mark && { color: colors.ink }]}>{String(mark).padStart(2, '0')}</Text></Pressable>
                <View pointerEvents="none" style={styles.depthRule} />
              </View>)}
              <View nativeID="depth-cursor" pointerEvents="none" style={[styles.depthCursor, { top: `${depth * 10}%` }]}><View style={styles.cursorTip} /><View style={styles.cursorLine} /></View>
              <View nativeID="field-reading" pointerEvents="none" style={styles.fieldReading}><Text nativeID="field-depth-value" style={styles.fieldNumber}>{depth.toFixed(1)}</Text><Text style={styles.fieldUnit}>m</Text></View>
            </View>
            <View style={styles.stageFooter}><Text style={styles.stageFootnote}>TAP THE SCALE</Text><Text style={styles.stageFootnote}>SIMULATED DEPTH</Text></View>
          </View>
          <View nativeID="device-dock" style={styles.deviceDock}>
            <View style={styles.deviceIdentity} accessibilityLiveRegion="polite">
              <View style={styles.connectionState}><View style={[styles.statusSquare, connected && { backgroundColor: colors.sea }, lost && { backgroundColor: colors.amber }]} /><Text style={styles.mono}>{connected ? 'CONNECTED / DEMO' : lost ? 'LINK LOST' : 'OFFLINE'}</Text></View>
              <Text nativeID="device-name" style={styles.deviceName}>{connected ? snapshot.deviceName : 'ESP32 WEARABLE'}</Text>
            </View>
            <View nativeID="device-actions" style={styles.deviceActions}>
              <ActionButton buttonRef={connectionActionRef} testID="connection-action" label={connected ? 'Disconnect' : lost ? 'Reconnect' : 'Connect'} onPress={connected ? service.disconnect : service.scan} icon={connected ? undefined : 'bluetooth'} secondary={connected} />
              {lost && <Pressable accessibilityRole="button" onPress={service.close} style={styles.dismiss}><Text style={styles.textLink}>Dismiss</Text></Pressable>}
            </View>
          </View>
          <View nativeID="scenario-panel" style={styles.scenarioSection}>
            <View style={styles.scenarioTitleRow}><Text style={styles.mono}>CONNECTION TEST</Text>{connected && <Pressable accessibilityRole="button" accessibilityLabel="Simulate connection loss" onPress={service.loseConnection} style={styles.dropLink}><Text style={styles.textLink}>DROP LINK ↗</Text></Pressable>}</View>
            <View nativeID="scenario-options" style={styles.scenarioOptions}>
              {scenarios.map(scenario => <Pressable key={scenario.value} accessibilityRole="button" accessibilityLabel={scenario.accessibleLabel} {...selectedButtonProps(snapshot.scenario === scenario.value)} onPress={() => service.setScenario(scenario.value)} style={[styles.scenarioOption, snapshot.scenario === scenario.value && styles.scenarioSelected]}><Text style={[styles.scenarioName, snapshot.scenario === scenario.value && styles.scenarioNameSelected]}>{scenario.label}</Text></Pressable>)}
            </View>
          </View>
        </View>
        <View nativeID="controls-panel" style={styles.controlsPanel}>
          <View nativeID="controls-heading" style={styles.controlsHeading}>
            <Text accessibilityRole="header" style={styles.sectionTitle}>DIAL IT IN.</Text>
            <Pressable accessibilityRole="button" accessibilityLabel="Reset demo values" onPress={() => { setHeartRate(70); setDepth(0); setResetMessage('Values reset'); }} style={styles.reset}><Icon name="reset" size={18} color={colors.sea} /><Text style={styles.textLink}>RESET</Text></Pressable>
          </View>
          <View nativeID="slider-stack"><DemoValueSlider kind="heart" value={heartRate} onChange={setHeartRate} /><DemoValueSlider kind="depth" value={depth} onChange={setDepth} /></View>
          <Text accessibilityLiveRegion="polite" style={styles.resetMessage}>{resetMessage || 'MANUAL INPUT / SESSION ONLY'}</Text>
        </View>
      </View>
      <View nativeID="sensor-strip" style={styles.sensorStrip}>
        <View style={styles.sensorIntro}><Text style={styles.mono}>SENSOR PREVIEW</Text><Text style={styles.sensorMode}>{connected ? 'SIMULATED' : 'OFFLINE'}</Text></View>
        <Sensor label="DEPTH" value={connected ? `${depth.toFixed(1)} m` : '—'} /><Sensor label="IMU" value="—" /><Sensor label="PPG" value="—" />
      </View>
      <LiveTelemetryPanel />
      <View style={styles.footer}><Text style={styles.footerText}>KΒ / BENCH MODE</Text><Text style={styles.footerText}>Demo values and live readings are separate.</Text></View>
    </View>
    <DevicePickerSheet snapshot={snapshot} service={service} />
  </Page>;
}

function Sensor({ label, value }: { label: string; value: string }) {
  return <View style={styles.sensor}><Text style={styles.mono}>{label}</Text><Text style={styles.sensorValue}>{value}</Text></View>;
}

const styles = StyleSheet.create({
  page: { backgroundColor: colors.sand, minHeight: '100%', width: '100%' },
  container: { width: '100%', maxWidth: 1340, alignSelf: 'center', paddingHorizontal: 32, paddingTop: 24, paddingBottom: 20 },
  header: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', flexWrap: 'wrap', gap: 20, paddingBottom: 22, marginBottom: 24, borderBottomWidth: 2, borderColor: colors.ink },
  wordmark: { flexDirection: 'row', alignItems: 'center', gap: 14, minWidth: 0, flexShrink: 1 },
  brand: { fontFamily: displayFont, fontSize: 52, lineHeight: 58, fontWeight: '700', letterSpacing: -1.2, color: colors.ink, flexShrink: 1 },
  headerMeta: { alignItems: 'flex-end', gap: 8, maxWidth: '100%' },
  mono: { fontFamily: monoFont, fontSize: 10, lineHeight: 17, color: colors.muted, letterSpacing: 0.5, flexShrink: 1 },
  demoBadge: { backgroundColor: colors.ink, paddingVertical: 5, paddingHorizontal: 9, maxWidth: '100%' },
  demoText: { fontFamily: monoFont, fontSize: 9, lineHeight: 15, color: colors.action },
  dashboard: { gap: 28 }, dashboardWide: { flexDirection: 'row', alignItems: 'stretch' }, fieldColumn: { flex: 1.1, minWidth: 0 },
  stage: { minHeight: 452, backgroundColor: colors.ink, padding: 24, overflow: 'hidden' },
  stageImage: { position: 'absolute', inset: 0, height: '100%', width: '100%', opacity: 0.85 },
  stageShade: { position: 'absolute', inset: 0, backgroundColor: 'rgba(5,29,38,0.34)' },
  stageHeader: { flexDirection: 'row', justifyContent: 'space-between', flexWrap: 'wrap', gap: 8 },
  stageLabel: { fontFamily: monoFont, fontSize: 10, lineHeight: 17, letterSpacing: 1, color: '#EFF4E9' },
  depthField: { flex: 1, minHeight: 280, marginTop: 36, marginBottom: 36 },
  depthMarkRow: { position: 'absolute', left: 0, right: 0, height: 48, transform: [{ translateY: -24 }], flexDirection: 'row', alignItems: 'center', gap: 12 },
  depthMark: { width: 48, height: 48, justifyContent: 'center', alignItems: 'center', borderWidth: 1, borderColor: 'transparent' },
  depthMarkSelected: { backgroundColor: colors.action }, depthMarkText: { fontFamily: monoFont, fontSize: 13, color: '#F0F4E9' },
  depthRule: { height: 1, flex: 1, backgroundColor: 'rgba(239,244,233,0.2)' },
  depthCursor: { position: 'absolute', left: 60, right: 0, flexDirection: 'row', alignItems: 'center', transform: [{ translateY: -4 }] },
  cursorTip: { width: 8, height: 8, backgroundColor: colors.action, transform: [{ rotate: '45deg' }] }, cursorLine: { height: 2, flex: 1, backgroundColor: colors.action },
  fieldReading: { position: 'absolute', right: 0, bottom: 15, flexDirection: 'row', alignItems: 'baseline', gap: 8, maxWidth: '70%' },
  fieldNumber: { fontFamily: monoFont, fontSize: 72, lineHeight: 90, letterSpacing: -6, color: '#F4F6ED' }, fieldUnit: { fontFamily: monoFont, fontSize: 18, color: '#F4F6ED' },
  stageFooter: { flexDirection: 'row', justifyContent: 'space-between', flexWrap: 'wrap', gap: 8 }, stageFootnote: { fontFamily: monoFont, fontSize: 9, lineHeight: 16, color: '#EFF4E9' },
  deviceDock: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', flexWrap: 'wrap', gap: 16, paddingVertical: 20, borderBottomWidth: 1, borderColor: colors.line },
  deviceIdentity: { flex: 1, minWidth: 0, gap: 6 }, connectionState: { flexDirection: 'row', alignItems: 'center', gap: 7 }, statusSquare: { width: 6, height: 6, backgroundColor: colors.muted, flexShrink: 0 },
  deviceName: { fontFamily: displayFont, fontSize: 25, lineHeight: 29, fontWeight: '700', color: colors.ink }, deviceActions: { minWidth: 138, maxWidth: '100%', gap: 4 },
  dismiss: { minHeight: 48, justifyContent: 'center', alignItems: 'center' }, textLink: { fontFamily: monoFont, fontSize: 10, lineHeight: 18, color: colors.sea, flexShrink: 1 },
  scenarioSection: { paddingTop: 12, gap: 8 }, scenarioTitleRow: { minHeight: 30, flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', flexWrap: 'wrap', gap: 8 }, dropLink: { minHeight: 48, paddingHorizontal: 8, justifyContent: 'center' },
  scenarioOptions: { flexDirection: 'row', gap: 0 }, scenarioOption: { flex: 1, minHeight: 48, borderWidth: 1, borderColor: colors.line, justifyContent: 'center', alignItems: 'center', padding: 8 }, scenarioSelected: { backgroundColor: colors.ink, borderColor: colors.ink },
  scenarioName: { fontFamily: monoFont, fontSize: 11, lineHeight: 18, color: colors.ink }, scenarioNameSelected: { color: colors.white },
  controlsPanel: { flex: 1, minWidth: 0, backgroundColor: colors.card, padding: 28, borderWidth: 1, borderColor: colors.line },
  controlsHeading: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', flexWrap: 'wrap', gap: 12, paddingBottom: 16, borderBottomWidth: 2, borderColor: colors.ink }, sectionTitle: { fontFamily: displayFont, fontSize: 36, lineHeight: 40, fontWeight: '700', color: colors.ink },
  reset: { flexDirection: 'row', alignItems: 'center', gap: 8, minHeight: 48, maxWidth: '100%' }, resetMessage: { fontFamily: monoFont, fontSize: 9, lineHeight: 16, color: colors.muted, paddingTop: 16 },
  sensorStrip: { flexDirection: 'row', flexWrap: 'wrap', gap: 0, marginTop: 28, borderTopWidth: 2, borderBottomWidth: 1, borderColor: colors.ink },
  sensorIntro: { minWidth: 180, flex: 1.5, justifyContent: 'center', paddingVertical: 20, gap: 4 }, sensorMode: { fontFamily: monoFont, fontSize: 9, lineHeight: 16, color: colors.sea },
  sensor: { flex: 1, minWidth: 90, paddingVertical: 20, paddingLeft: 20, borderLeftWidth: 1, borderColor: colors.line, gap: 6 }, sensorValue: { fontFamily: monoFont, fontSize: 22, lineHeight: 30, color: colors.ink },
  footer: { flexDirection: 'row', justifyContent: 'space-between', flexWrap: 'wrap', gap: 10, marginTop: 16 }, footerText: { fontFamily: monoFont, fontSize: 9, lineHeight: 16, color: colors.muted },
});
