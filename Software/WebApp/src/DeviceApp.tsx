import { Image } from 'react-native';
import { useEffect, useId, useRef, useState, useSyncExternalStore, type KeyboardEvent, type ReactNode } from 'react';
import { createLiveConnectionService, type LiveStatus } from './services/liveConnection';
import { createIndexedDbHistoryStore } from './services/historyStore';
import { HistoryView } from './components/HistoryView.web';
import { useTheme } from './useTheme';
import { defaultPreferences, displayWeight, weightToKg, parsePreferences, preferenceKey, type Preferences } from './services/preferences';
import { profileMatchesPreferences } from './services/profileProtocol';
import './device-app.css';

const oceanImage = require('../assets/ocean-background.png');
const oceanStyle = { position: 'absolute' as const, inset: 0, width: '100%' as const, height: '100%' as const, opacity: 0.18, zIndex: -1 };

type Tab = 'connect' | 'settings' | 'live';
type Sensor = 'Depth' | 'Vitals' | 'Motion' | 'Device' | 'History';
function readPreferences(): Preferences | null {
  try { return parsePreferences(localStorage.getItem(preferenceKey)); }
  catch { return null; }
}
const statusLabels: Record<LiveStatus, string> = {
  disconnected: 'Not connected', selecting: 'Choose your device', connecting: 'Connecting…', syncing: 'Connected · syncing',
  connected: 'Connected', lost: 'Connection lost', error: 'Connection error', unsupported: 'Bluetooth unavailable',
};
const number = (n: number | null | undefined, decimals = 0) => n == null ? '—' : n.toFixed(decimals);
const tabs: Tab[] = ['connect', 'settings', 'live'];
const sensors: Sensor[] = ['Depth', 'Vitals', 'Motion', 'Device', 'History'];
const helpContent = {
  heartRate: { title: 'Resting heart rate', paragraphs: ['Your usual heart rate at rest, in beats per minute. This is a baseline you enter, not a live sensor reading.', 'Saved settings transfer to compatible firmware on the next connection. They do not control device triggers yet, and the input range is not a validated device operating limit.'] },
  maxDepth: { title: 'Estimated maximum depth', paragraphs: ['The maximum depth you intend to set, in meters. The intended future device behavior is to trigger beyond this depth.', 'Saved settings transfer to compatible firmware on the next connection. Current firmware does not enforce this threshold, and the input range is not a validated device operating limit.'] },
  weightKg: { title: 'Weight', paragraphs: ['Your body weight in kilograms or pounds. Choose kg or lbs; switching units converts the displayed value. Adjust in 0.1-unit steps.', 'Saved settings transfer on the next connection. Weight is sent in kilograms, rounded to 0.1 kg. It does not control device triggers yet.'] },
  heightCm: { title: 'Height', paragraphs: ['Your height in centimeters. Adjust in 1 cm steps.', 'Saved settings transfer to compatible firmware on the next connection. It does not control device triggers yet.'] },
  settings: { title: 'Saving your settings', paragraphs: ['Save to browser keeps your heart-rate baseline, maximum depth, weight and height for your next visit. On the next connection, the app sends the latest saved settings after history finishes and reads them back to confirm receipt.', 'Unsaved edits are never sent. With no saved settings, transfer is skipped. Older firmware and the demo may not support profiles. Receipt does not mean the settings control device triggers or persist through a device restart.'] },
  connection: { title: 'Connect your wearable', paragraphs: ['Use Chrome on Windows, macOS or Android over HTTPS or localhost. Select your KappaBeta wearable or KappaBetaDemo in the Bluetooth picker. iPhone Safari cannot connect through this Web Bluetooth interface.', 'Saved history downloads when available; KappaBetaDemo streams live readings without recording. Keep the sensor at the water surface when zeroing. This app does not run automatic drowning detection or remotely operate the pump.'] },
  Depth: { title: 'About depth readings', paragraphs: ['Depth is calculated from water pressure relative to your surface baseline. In Connect, hold the sensor at the water surface and choose Zero depth at surface.', 'Pressure is shown in pascals and water temperature in degrees Celsius. A dash means no current valid reading is available.'] },
  Vitals: { title: 'About vital readings', paragraphs: ['Heart rate and oxygen estimates come from the wearable’s optical sensor. Both require a valid optical signal.', 'Red and infrared are raw optical readings. A dash means no current valid reading is available.'] },
  Motion: { title: 'About motion readings', paragraphs: ['X, Y and Z are the sensor’s three measurement axes. Acceleration is shown in g and includes gravity. Rotation is shown in degrees per second.', 'A dash means no current valid reading is available.'] },
  Device: { title: 'About device status', paragraphs: ['Pump, vibration motor and blue LED values show commanded states, not measured water flow. The displayed countdown output matches your firmware. Use the physical Cancel button on the wearable to stop the pump.', 'Sensor errors and optical overflows are diagnostic counts reported by the wearable. A dash means no current valid reading is available.'] },
};
function nextTab(key: string, index: number, count: number) {
  return key === 'ArrowRight' ? (index + 1) % count : key === 'ArrowLeft' ? (index + count - 1) % count
    : key === 'Home' ? 0 : key === 'End' ? count - 1 : -1;
}

export default function DeviceApp() {
  const { theme, toggleTheme } = useTheme();
  const [store] = useState(() => createIndexedDbHistoryStore());
  const [service] = useState(() => createLiveConnectionService({ historyStore: store, getSavedPreferences: readPreferences }));
  const snapshot = useSyncExternalStore(service.subscribe, service.getSnapshot, service.getSnapshot);
  const [frozen, setFrozen] = useState<typeof snapshot | null>(null);
  const [tab, setTab] = useState<Tab>('connect');
  const [sensor, setSensor] = useState<Sensor>('Depth');
  const [saved, setSaved] = useState(readPreferences);
  const [draft, setDraft] = useState<Preferences>(() => saved ?? { ...defaultPreferences });
  const [setting, setSetting] = useState<keyof Preferences>('heartRate');
  const [saveMessage, setSaveMessage] = useState('');
  const [help, setHelp] = useState<keyof typeof helpContent | null>(null);
  const dialog = useRef<HTMLDialogElement>(null);
  const returnFocus = useRef<HTMLElement | null>(null);
  useEffect(() => () => service.disconnect(), [service]);
  useEffect(() => { if (!['connected', 'syncing'].includes(snapshot.status)) setFrozen(null); }, [snapshot.status]);
  useEffect(() => { if (snapshot.profileSync.status !== 'idle') setSaveMessage(''); }, [snapshot.profileSync.status]);
  useEffect(() => { document.documentElement.lang = 'en'; document.title = 'Kappa Beta · Wearable'; }, []);
  useEffect(() => {
    if (help) { returnFocus.current = document.activeElement as HTMLElement; dialog.current?.showModal(); document.getElementById('help-title')?.focus(); }
    else { dialog.current?.close(); returnFocus.current?.focus(); }
  }, [help]);
  const connected = snapshot.status === 'connected';
  const linked = connected || snapshot.status === 'syncing';
  const busy = ['selecting', 'connecting', 'syncing'].includes(snapshot.status);
  const t = (frozen ?? snapshot).telemetry;
  const depth = t.depth && !t.depth.stale ? t.depth.data : null;
  const imu = t.imu && !t.imu.stale ? t.imu.data : null;
  const ppg = t.ppg && !t.ppg.stale ? t.ppg.data : null;
  const system = t.system && !t.system.stale ? t.system.data : null;
  const indicatorLabel = system?.vibrationMotor ? 'Vibration motor' : 'Blue LED';
  const pumpStatus = !system ? '—' : system.countdownActive ? `${Math.ceil(system.remainingMs / 1000)}s countdown`
    : system.pumpOn ? system.remainingMs > 0 ? `Running · ${Math.ceil(system.remainingMs / 1000)}s left` : 'Running'
      : system.pumpEnabled ? 'Stopped' : 'Disabled';
  const stale = Object.values(t).some(value => value?.stale);
  const dirty = !saved || draft.heartRate !== saved.heartRate || draft.maxDepth !== saved.maxDepth || draft.weightKg !== saved.weightKg || draft.heightCm !== saved.heightCm || draft.weightUnit !== saved.weightUnit;
  const profilePending = saved !== null && (snapshot.profileSync.status === 'not-saved'
    || (snapshot.profileSync.status === 'synced' && !profileMatchesPreferences(snapshot.profileSync.confirmedProfile, saved)));
  const profileMessage = profilePending ? 'New saved settings pending. Reconnect to send them.' : snapshot.profileSync.message;
  const history = snapshot.history;
  const validBaseline = connected && depth?.depthM != null;

  function navigate(next: Tab) {
    if (next !== 'live') setFrozen(null);
    setTab(next);
    document.getElementById(`tab-${next}`)?.focus();
  }
  function tabKeys(event: KeyboardEvent<HTMLButtonElement>, index: number) {
    const next = nextTab(event.key, index, tabs.length);
    if (next >= 0) { event.preventDefault(); navigate(tabs[next]); document.getElementById(`tab-${tabs[next]}`)?.focus(); }
  }
  function selectSensor(next: Sensor) { setSensor(next); document.getElementById(`sensor-${next}`)?.focus(); }
  function sensorKeys(event: KeyboardEvent<HTMLButtonElement>, index: number) {
    const next = nextTab(event.key, index, sensors.length);
    if (next >= 0) { event.preventDefault(); selectSensor(sensors[next]); }
  }
  function save() {
    try { localStorage.setItem(preferenceKey, JSON.stringify(draft)); setSaved({ ...draft }); setSaveMessage('Saved. Will send on your next connection.'); }
    catch { setSaveMessage('Could not save. Browser storage may be unavailable.'); }
  }
  const change = <K extends keyof Preferences,>(key: K, value: Preferences[K]) => { setDraft(p => ({ ...p, [key]: value })); setSaveMessage(''); };

  return <div className="device-app">
    <a className="skip-link" href="#workspace" onClick={event => { event.preventDefault(); document.getElementById('workspace')?.focus(); }}>Skip to content</a>
    <header className="app-header">
      <a className="wordmark" href="#connect" onClick={e => { e.preventDefault(); navigate('connect'); }} aria-label="Kappa Beta home">KAPPA<span> / </span>BETA</a>
      <div className="header-actions">
      <button className={`connection-badge ${linked ? 'is-connected' : ''}`} onClick={() => navigate('connect')} aria-label={`Device status: ${statusLabels[snapshot.status]}. Open connection tab`}>
        <span className={busy ? 'status-dot is-pulsing' : 'status-dot'} aria-hidden="true" />
        <span role="status">{statusLabels[snapshot.status]}</span>
      </button>
      <button className="theme-toggle" aria-pressed={theme === 'dark'} onClick={toggleTheme}><span className="theme-switch" aria-hidden="true" />Dark mode</button>
      </div>
    </header>

    <nav className="main-tabs" role="tablist" aria-label="Wearable workspace">
      {tabs.map((name, index) => <button key={name} id={`tab-${name}`} role="tab" aria-selected={tab === name} aria-controls={`panel-${name}`}
        tabIndex={tab === name ? 0 : -1} onKeyDown={e => tabKeys(e, index)} onClick={() => navigate(name)}>
        <span className="tab-number" aria-hidden="true">0{index + 1}</span><span>{name === 'connect' ? 'Connect' : name === 'settings' ? 'Settings' : 'Live'}</span>
      </button>)}
    </nav>

    <main className="workspace" id="workspace" tabIndex={-1}>
      <section id="panel-connect" className="tab-panel connect-panel" role="tabpanel" aria-labelledby="tab-connect" tabIndex={0} hidden={tab !== 'connect'}>
        <div className="ocean-stage"><Image source={oceanImage} resizeMode="cover" accessible={false} style={oceanStyle} />
          <div className="stage-top"><span>YOUR WEARABLE. YOUR WATER.</span></div>
          <p className="ocean-heading">Before<br />you dive.</p>
          <div className="stage-bottom"><span>Establish your<br />surface reference.</span><span className="water-symbol" aria-hidden="true">≈</span></div>
        </div>
        <div className="connect-controls">
          <div className="section-title"><h1>Connect your device</h1><button className="help-button" aria-label="Bluetooth connection help" onClick={() => setHelp('connection')}>?</button></div>
          <div className="connection-group"><div className="device-card">
            <div className="device-card-copy"><h2>{snapshot.deviceName || 'KappaBeta wearable'}</h2>
              <div id="connection-instruction" className="connection-feedback" role="status">
                {snapshot.message || (snapshot.status === 'syncing' ? `Importing history${history ? ` · ${history.recordCount.toLocaleString()} records` : ''}.`
                  : connected ? 'Connected. Ready for a surface baseline.' : 'Turn on your wearable and keep it nearby.')}
              </div>
            </div>
            <button aria-describedby="connection-instruction" className={linked || busy ? 'button outline' : 'button primary'} onClick={linked || busy ? service.disconnect : () => { void service.connect(); }}>
              {busy ? 'Cancel' : connected ? 'Disconnect' : snapshot.status === 'lost' ? 'Reconnect' : 'Connect wearable'}
            </button>
          </div></div>
          {profileMessage && <p className="profile-sync-feedback" role="status">{profileMessage}</p>}
          <div className="baseline-card">
            <div className="baseline-heading"><h2>Depth from surface</h2><span className="state-label">{validBaseline ? 'Available' : 'Not set'}</span></div>
            <div className="baseline-value"><strong>{number(depth?.depthM, 2)}</strong><span>m</span></div>
            <div className="baseline-action"><p id="baseline-instruction">Hold the sensor at the water surface.</p>
            <button className="button dark" aria-describedby="baseline-instruction" disabled={!connected || snapshot.zeroPending} onClick={() => { void service.zeroDepth(); }}>{snapshot.zeroPending ? 'Requesting…' : 'Zero depth at surface'}</button></div>
          </div>
          <button className="button outline next-step" onClick={() => navigate('settings')}>Open settings</button>
        </div>
      </section>

      <section id="panel-settings" className="tab-panel settings-panel" role="tabpanel" aria-labelledby="tab-settings" tabIndex={0} hidden={tab !== 'settings'}>
        <div className="page-heading"><div><span className="eyebrow">PERSONAL SETUP</span><h1>Your settings.</h1></div><span className="state-label">{dirty ? 'Unsaved changes' : 'Saved on this browser'}</span></div>
        <div className="setting-switch" role="group" aria-label="Choose setting"><button aria-pressed={setting === 'heartRate'} onClick={() => setSetting('heartRate')}>Heart rate</button><button aria-pressed={setting === 'maxDepth'} onClick={() => setSetting('maxDepth')}>Max. depth</button><button aria-pressed={setting === 'weightKg'} onClick={() => setSetting('weightKg')}>Weight</button><button aria-pressed={setting === 'heightCm'} onClick={() => setSetting('heightCm')}>Height</button></div>
        <div className="settings-grid">
          <SettingCard name="Resting heart rate" unit="bpm" value={draft.heartRate} min={20} max={140} step={1} active={setting === 'heartRate'} onHelp={() => setHelp('heartRate')} onChange={v => change('heartRate', v)} />
          <SettingCard name="Estimated maximum depth" unit="m" value={draft.maxDepth} min={0.1} max={10} step={0.1} active={setting === 'maxDepth'} onHelp={() => setHelp('maxDepth')} onChange={v => change('maxDepth', v)} />
          <SettingCard name="Weight" unit={draft.weightUnit} value={displayWeight(draft.weightKg, draft.weightUnit)} min={displayWeight(20, draft.weightUnit)} max={displayWeight(150, draft.weightUnit)} step={0.1} active={setting === 'weightKg'} onHelp={() => setHelp('weightKg')} onChange={v => change('weightKg', weightToKg(v, draft.weightUnit))}
            controls={<div className="weight-unit-switch" role="group" aria-label="Weight unit"><button aria-label="Kilograms" aria-pressed={draft.weightUnit === 'kg'} onClick={() => change('weightUnit', 'kg')}>kg</button><button aria-label="Pounds" aria-pressed={draft.weightUnit === 'lbs'} onClick={() => change('weightUnit', 'lbs')}>lbs</button></div>} />
          <SettingCard name="Height" unit="cm" value={draft.heightCm} min={80} max={250} step={1} active={setting === 'heightCm'} onHelp={() => setHelp('heightCm')} onChange={v => change('heightCm', v)} />
        </div>
        <div className="settings-footer"><div className="save-context"><div className="save-scope"><span>Saved here · sent on next connection</span><button className="help-button" aria-label="About saving settings" onClick={() => setHelp('settings')}>?</button></div><span className="save-feedback" role="status">{profilePending ? profileMessage : saveMessage || profileMessage}</span></div>
          <div className="setting-actions"><button className="button outline" disabled={!dirty} onClick={() => { setDraft(saved ?? { ...defaultPreferences }); setSaveMessage('Edits reset.'); }}>Reset edits</button><button className="button primary" disabled={!dirty} onClick={save}>Save to browser</button></div>
        </div>
      </section>

      <section id="panel-live" className="tab-panel live-panel" role="tabpanel" aria-labelledby="tab-live" tabIndex={0} hidden={tab !== 'live'}>
        <div className="page-heading"><div><span className="eyebrow">{sensor === 'History' ? 'SAVED ON THIS BROWSER' : 'DIRECT FROM YOUR ESP32'}</span><h1>{sensor === 'History' ? 'Your recordings.' : 'In the water.'}</h1></div><div className="live-state" hidden={sensor === 'History'}><span className="stream-state" role="status">{frozen ? history?.phase === 'live-only' ? 'Display paused · live stream continues' : 'Display paused · recording continues' : connected ? stale ? 'Some readings are stale' : history?.phase === 'live-only' ? 'Live only · no recording' : 'Receiving sensor data' : snapshot.status === 'syncing' ? 'Importing history' : 'No live data'}</span>{linked ? <button className="pause-control" onClick={() => setFrozen(frozen ? null : snapshot)}>{frozen ? 'Resume updates' : 'Pause updates'}</button> : <button className="pause-control" onClick={() => navigate('connect')}>Open connection</button>}</div></div>
        <div className="sensor-tabs" role="tablist" aria-label="Sensor views">{sensors.map((name, index) => <button key={name} id={`sensor-${name}`} role="tab" aria-selected={sensor === name} aria-controls={`sensor-panel-${name}`} tabIndex={sensor === name ? 0 : -1} onKeyDown={event => sensorKeys(event, index)} onClick={() => selectSensor(name)}>{name}</button>)}</div>
        <div className="sensor-content">
          <div className="depth-view" id="sensor-panel-Depth" role="tabpanel" aria-labelledby="sensor-Depth" tabIndex={0} hidden={sensor !== 'Depth'}>
            <div className="live-ocean"><Image source={oceanImage} resizeMode="cover" accessible={false} style={oceanStyle} /><span>DEPTH FROM SURFACE</span><div className="hero-reading">{number(depth?.depthM, 2)}<span>m</span></div>{t.depth?.stale ? <span>Reading stale</span> : depth?.pressurePa != null && depth.depthM == null ? <button className="surface-action" onClick={() => navigate('connect')}>Set surface baseline</button> : null}</div>
            <div className="depth-details"><Metric label="Pressure" value={number(depth?.pressurePa)} unit="Pa" /><Metric label="Water temperature" value={number(depth?.temperatureC, 1)} unit="°C" /></div>
          </div>
          <div className="vitals-view metric-grid" id="sensor-panel-Vitals" role="tabpanel" aria-labelledby="sensor-Vitals" tabIndex={0} hidden={sensor !== 'Vitals'}><Metric label="Heart rate" value={number(ppg?.heartRateBpm)} unit="bpm" /><Metric label="Oxygen estimate" value={number(ppg?.spo2Percent, 1)} unit="%" /><div className="raw-reading"><span>Raw optical · red / infrared</span><strong>{number(ppg?.red)} / {number(ppg?.infrared)}</strong>{t.ppg?.stale && <p>Reading stale.</p>}</div></div>
          <div className="motion-view" id="sensor-panel-Motion" role="tabpanel" aria-labelledby="sensor-Motion" tabIndex={0} hidden={sensor !== 'Motion'}><Axes title="Acceleration" values={imu?.accelerationG} unit="g" decimals={3} /><Axes title="Rotation" values={imu?.angularVelocityDps} unit="°/s" decimals={1} />{t.imu?.stale && <p>Reading stale.</p>}</div>
          <div className="device-view metric-grid" id="sensor-panel-Device" role="tabpanel" aria-labelledby="sensor-Device" tabIndex={0} hidden={sensor !== 'Device'}><Metric label="Battery" value={number(system?.batteryV, 2)} unit="V" /><div className="actuator-card"><span>Pump command</span><strong>{pumpStatus}</strong><span>{indicatorLabel} {system ? system.vibrationOn ? 'on' : 'off' : '—'}</span><p className="control-note">Use physical Cancel on the wearable to stop the pump.</p></div><dl className="diagnostic-readings"><div><dt>Sensor errors</dt><dd>{system?.sensorErrors ?? '—'}</dd></div><div><dt>Optical overflows</dt><dd>{system?.ppgOverflowCount ?? '—'}</dd></div></dl></div>
          <div className="history-container" id="sensor-panel-History" role="tabpanel" aria-labelledby="sensor-History" tabIndex={0} hidden={sensor !== 'History'}><HistoryView store={store} progress={(frozen ?? snapshot).history} connected={linked} paused={frozen !== null} onConnect={() => navigate('connect')} /></div>
        </div>
        <div className="live-footer" hidden={sensor === 'History'}><button className="button outline" onClick={() => { if (sensor !== 'History') setHelp(sensor); }}>About {sensor.toLowerCase()}</button><button className="button outline" onClick={() => selectSensor('History')}>History & export</button></div>
      </section>
    </main>
    <dialog ref={dialog} className="help-dialog" onCancel={e => { e.preventDefault(); setHelp(null); }} onClick={e => { if (e.target === e.currentTarget) setHelp(null); }} aria-labelledby="help-title">
      <div className="section-title"><h2 id="help-title" tabIndex={-1}>{help ? helpContent[help].title : ''}</h2><button className="help-button" aria-label="Close help" onClick={() => setHelp(null)}>×</button></div>
      {help && helpContent[help].paragraphs.map(paragraph => <p key={paragraph}>{paragraph}</p>)}
      <button className="button primary" onClick={() => setHelp(null)}>Got it</button>
    </dialog>
  </div>;
}

function SettingCard({ name, unit, value, min, max, step, active, onHelp, onChange, controls }: { controls?: ReactNode; name: string; unit: string; value: number; min: number; max: number; step: number; active: boolean; onHelp: () => void; onChange: (value: number) => void }) {
  const id = useId();
  const change = (next: number) => onChange(Math.round(Math.min(max, Math.max(min, next)) * 10) / 10);
  return <article className={`setting-card ${active ? 'active-setting' : ''}`}>
    <div className="section-title"><h2>{name}</h2><button className="help-button" onClick={onHelp} aria-label={`About ${name.toLowerCase()}`}>?</button></div>
    {controls}
    <div className="setting-value"><output htmlFor={id} aria-live="off">{value.toFixed(step < 1 ? 1 : 0)}</output><span>{unit}</span></div>
    <p id={`${id}-description`} className="sr-only">User-entered setting. Save to browser to send it on the next connection. Settings do not control device triggers yet. Use the slider or the decrease and increase buttons.</p>
    <div className="slider-controls"><button aria-label={`Decrease ${name.toLowerCase()}`} disabled={value <= min} onClick={() => change(value - step)}>−</button><input id={id} type="range" aria-label={name} aria-describedby={`${id}-description`} aria-valuetext={`${value.toFixed(step < 1 ? 1 : 0)} ${unit === 'bpm' ? 'beats per minute' : unit === 'kg' ? 'kilograms' : unit === 'lbs' ? 'pounds' : unit === 'cm' ? 'centimeters' : 'meters'}`} min={min} max={max} step={step} value={value} onChange={e => change(Number(e.target.value))} style={{ '--range-fill': `${(value - min) / (max - min) * 100}%` } as React.CSSProperties} /><button aria-label={`Increase ${name.toLowerCase()}`} disabled={value >= max} onClick={() => change(value + step)}>+</button></div>
    <div className="range-labels"><span>{min} {unit}</span><span>{max} {unit}</span></div>
  </article>;
}
function Metric({ label, value, unit }: { label: string; value: string; unit: string }) { return <article className="metric"><h2>{label}</h2><div><strong>{value}</strong><span>{unit}</span></div></article>; }
function Axes({ title, values, unit, decimals }: { title: string; values?: [number, number, number] | null; unit: string; decimals: number }) { return <div className="axes"><h2>{title} <span>{unit}</span></h2><div>{['X', 'Y', 'Z'].map((axis, i) => <div key={axis}><span>{axis}</span><strong>{number(values?.[i], decimals)}</strong></div>)}</div></div>; }
