import { useEffect, useState } from 'react';
import { Pressable, StyleSheet, Text, View } from 'react-native';
import { exportSessionCsv } from '../services/historyExport';
import { type HistorySession, type HistoryStore } from '../services/historyStore';
import { type HistoryProgress } from '../services/historyTransfer';
import { colors } from '../theme';

export function SavedHistoryPanel({ store, progress, connected }: { store: HistoryStore; progress: HistoryProgress | null; connected: boolean }) {
  const [sessions, setSessions] = useState<HistorySession[]>([]);
  const [message, setMessage] = useState('');
  const [exporting, setExporting] = useState<string | null>(null);
  const [parts, setParts] = useState<Record<string, number>>({});
  const [expanded, setExpanded] = useState(false);
  useEffect(() => {
    let active = true;
    const refresh = () => { void store.listSessions().then(value => { if (active) setSessions(value); })
      .catch(error => { if (active) setMessage(error instanceof Error ? error.message : 'Cannot read saved history.'); }); };
    refresh();
    const timer = connected ? setInterval(refresh, 2000) : undefined;
    return () => { active = false; if (timer) clearInterval(timer); };
  }, [store, connected]);

  async function exportCsv(session: HistorySession) {
    if (exporting) return;
    setExporting(session.key); setMessage('Preparing CSV…');
    try {
      // Calling immediately preserves the gesture needed by the save-file picker.
      const result = await exportSessionCsv(store, session, parts[session.key] ?? -1,
        count => setMessage(`Exporting ${count.toLocaleString()} entries…`));
      setParts(previous => { const next = { ...previous }; if (result.complete) delete next[session.key]; else next[session.key] = result.nextSequence; return next; });
      setMessage(result.complete ? `Exported ${result.entries.toLocaleString()} entries.`
        : `Exported ${result.entries.toLocaleString()} entries in this CSV part. Select “Export next CSV part” to continue.`);
    } catch (error) {
      setMessage(error instanceof Error && error.name === 'AbortError' ? 'CSV export cancelled.' : error instanceof Error ? error.message : 'CSV export failed.');
    } finally { setExporting(null); }
  }
  const phase = progress?.phase;
  const liveOnly = phase === 'live-only';
  const saving = phase === 'opening' || phase === 'downloading' || phase === 'saving';
  const current = progress ? `${progress.recordCount.toLocaleString()} records saved · ${progress.missingCount.toLocaleString()} records unavailable` : '';
  const downloaded = progress?.target == null ? null : Math.max(0, progress.cursor - progress.start);
  const total = progress?.target == null ? null : Math.max(0, progress.target - progress.start);
  return <View style={styles.panel}>
    <Text style={styles.heading}>SENSOR HISTORY / THIS BROWSER</Text>
    {progress && <Text accessibilityLiveRegion="polite" style={styles.copy}>
      {liveOnly ? 'Live only: wearable history memory is unavailable. No history is being saved.'
        : phase === 'error' || !connected ? `History paused. ${current}. Reconnect to resume.`
          : saving ? `Saving before live: ${downloaded == null ? 'opening the session…' : `${downloaded.toLocaleString()} / ${total!.toLocaleString()} record positions`}`
            : `${connected ? phase === 'catching-up' ? 'Saving new samples. ' : 'Saving while connected. ' : ''}${current}.`}
    </Text>}
    {progress && !liveOnly && progress.missingCount > 0 && <Text style={styles.copy}>Missing ranges are retained as explicit gap entries in the saved session and CSV.</Text>}
    {progress && Boolean(progress.flags & 4) && <Text style={styles.copy}>This device session reached its sequence limit. History recording has stopped; existing records remain saved.</Text>}
    <Text style={styles.note}>History stays in this browser on this site. Reconnecting resumes its saved checkpoint. The wearable’s unsaved history is lost when it resets or loses power; its oldest records are overwritten when its memory fills. Clearing browser data removes saved sessions.</Text>
    {sessions.length === 0 ? <Text style={styles.note}>No sessions saved in this browser yet.</Text> : (expanded ? sessions : sessions.slice(0, 3)).map(session => <View key={session.key} style={styles.session}>
      <View style={styles.sessionCopy}>
        <Text style={styles.copy}>Device {session.deviceId} · session {session.bootId}</Text>
        <Text style={styles.note}>{session.recordCount.toLocaleString()} records · {session.missingCount.toLocaleString()} unavailable · saved {new Date(session.updatedAt).toLocaleString()}</Text>
      </View>
      <Pressable accessibilityRole="button" disabled={exporting !== null} accessibilityState={{ disabled: exporting !== null }}
        onPress={() => { void exportCsv(session); }} style={[styles.button, exporting !== null && styles.disabled]}>
        <Text style={styles.buttonText}>{exporting === session.key ? 'Exporting…' : parts[session.key] === undefined ? 'Export CSV' : 'Export next CSV part'}</Text>
      </Pressable>
    </View>)}
    {sessions.length > 3 && <Pressable accessibilityRole="button" onPress={() => setExpanded(value => !value)}><Text style={styles.buttonText}>{expanded ? 'Show recent sessions' : `Show all ${sessions.length} sessions`}</Text></Pressable>}
    {!!message && <Text accessibilityLiveRegion="polite" style={styles.copy}>{message}</Text>}
    <Text style={styles.note}>CSV uses device acquisition time in microseconds, not calendar time. PPG sample times are estimated. Export streams to a file when supported; other browsers download parts of up to 5,000 entries.</Text>
  </View>;
}
const styles = StyleSheet.create({
  panel: { borderWidth: 1, borderColor: colors.line, padding: 16, gap: 10 },
  heading: { fontSize: 11, fontWeight: '700', color: colors.sea, letterSpacing: 1 },
  copy: { fontSize: 13, lineHeight: 20, color: colors.ink },
  note: { fontSize: 12, lineHeight: 19, color: colors.muted },
  session: { flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: 12, borderTopWidth: 1, borderColor: colors.line, paddingTop: 10 },
  sessionCopy: { flexGrow: 1, flexBasis: 280, minWidth: 0 },
  button: { minHeight: 44, justifyContent: 'center', borderWidth: 1, borderColor: colors.sea, padding: 10 },
  buttonText: { fontSize: 12, lineHeight: 18, color: colors.sea }, disabled: { opacity: 0.4 },
});
