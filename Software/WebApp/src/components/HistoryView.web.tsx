import { useEffect, useId, useRef, useState } from 'react';
import { exportSessionCsv } from '../services/historyExport';
import { type HistorySession, type HistoryStore } from '../services/historyStore';
import { type HistoryProgress } from '../services/historyTransfer';
import './HistoryView.css';

export function HistoryView({ store, progress, connected, paused = false, onConnect }: {
  store: HistoryStore; progress: HistoryProgress | null; connected: boolean; paused?: boolean; onConnect: () => void;
}) {
  const [sessions, setSessions] = useState<HistorySession[]>([]);
  const [selectedKey, setSelectedKey] = useState<string | null>(null);
  const [message, setMessage] = useState('');
  const [storageError, setStorageError] = useState('');
  const [exporting, setExporting] = useState<string | null>(null);
  const [parts, setParts] = useState<Record<string, number>>({});
  const exportLock = useRef(false);
  const detail = useRef<HTMLDialogElement>(null);
  const detailTrigger = useRef<HTMLButtonElement>(null);
  const detailTitle = useRef<HTMLHeadingElement>(null);
  const titleId = useId();
  useEffect(() => {
    if (paused) return;
    let active = true;
    const refresh = () => { void store.listSessions().then(value => {
      if (!active) return;
      setSessions(value);
      setStorageError('');
      setSelectedKey(key => value.some(session => session.key === key) ? key : value[0]?.key ?? null);
    }).catch(error => { if (active) setStorageError(error instanceof Error ? error.message : 'Cannot read saved history.'); }); };
    refresh();
    const timer = connected ? setInterval(refresh, 2000) : undefined;
    return () => { active = false; if (timer) clearInterval(timer); };
  }, [store, connected, paused]);

  const index = Math.max(0, sessions.findIndex(session => session.key === selectedKey));
  const session = sessions[index];
  const activeSession = session?.key === progress?.sessionKey;
  const phase = progress?.phase;
  const transferring = phase === 'opening' || phase === 'downloading' || phase === 'saving';
  const state = storageError ? 'Storage unavailable'
    : phase === 'live-only' && connected ? 'Live only · no wearable history memory'
      : phase === 'error' ? 'Transfer stopped · reconnect to resume'
        : !connected ? 'Stored in this browser'
          : transferring ? 'Saving history before live readings'
            : phase === 'catching-up' ? 'Saving new samples' : 'Saving while connected';
  const saved = activeSession && progress ? Math.max(session.recordCount, progress.recordCount) : session?.recordCount ?? 0;
  const missing = activeSession && progress ? Math.max(session.missingCount, progress.missingCount) : session?.missingCount ?? 0;

  async function exportCsv(selected: HistorySession) {
    if (exportLock.current) return;
    exportLock.current = true;
    setExporting(selected.key);
    setMessage('Preparing CSV…');
    try {
      const result = await exportSessionCsv(store, selected, parts[selected.key] ?? -1,
        count => setMessage(`Exporting ${count.toLocaleString()} entries…`));
      setParts(previous => {
        const next = { ...previous };
        if (result.complete) delete next[selected.key]; else next[selected.key] = result.nextSequence;
        return next;
      });
      setMessage(result.complete ? `Exported ${result.entries.toLocaleString()} entries.` : 'Part saved. Export the next CSV part to continue.');
    } catch (error) {
      setMessage(error instanceof Error && error.name === 'AbortError' ? 'Export cancelled.'
        : error instanceof Error ? error.message : 'CSV export failed.');
    } finally { exportLock.current = false; setExporting(null); }
  }

  return <section className="history-view" aria-label="Saved sensor history">
    <div className="history-view__top">
      <h2>Saved history</h2>
      <button ref={detailTrigger} type="button" className="history-view__info" onClick={() => { detail.current?.showModal(); detailTitle.current?.focus(); }} aria-label="About saved history">?</button>
    </div>
    {(session || connected || storageError) && <p className="history-view__state">{state}</p>}
    {storageError ? <p role="alert" className="history-view__error">{storageError}</p> : <>
      {session ? <div className="history-view__stat">
        <strong>{saved.toLocaleString()}</strong><span>records saved</span>
      </div> : <div className="history-view__empty"><strong>{connected ? 'Waiting for samples' : 'No recordings yet'}</strong><p>{connected ? 'Imported recordings will appear here.' : 'Connect your wearable to import its recordings.'}</p>{!connected && <button className="button outline" onClick={onConnect}>Open connection</button>}</div>}
      {session && <p className="history-view__gaps">{missing ? `${missing.toLocaleString()} missing · gaps included in CSV` : 'No reported gaps'}</p>}
      {progress && Boolean(progress.flags & 4) && <p className="history-view__error">Recording stopped: sequence limit reached.</p>}
      {session && <>
        <nav className="history-view__sessions" aria-label="Saved sessions">
          <button type="button" disabled={index === 0 || exporting !== null} aria-label="Previous saved session" onClick={() => setSelectedKey(sessions[index - 1].key)}>Previous</button>
          <div><span>Session {index + 1} / {sessions.length}</span><time dateTime={session.createdAt}>{new Date(session.createdAt).toLocaleDateString()}</time></div>
          <button type="button" disabled={index >= sessions.length - 1 || exporting !== null} aria-label="Next saved session" onClick={() => setSelectedKey(sessions[index + 1].key)}>Next</button>
        </nav>
        <div className="history-view__export-area"><button type="button" className="history-view__export" disabled={exporting !== null} onClick={() => { void exportCsv(session); }}>
          {exporting === session.key ? 'Exporting…' : parts[session.key] === undefined ? 'Export CSV' : 'Export next CSV part'}
        </button><p>Export a copy of this browser’s saved readings.</p></div>
      </>}
    </>}
    <p className="history-view__message" role="status">{message}</p>
    <dialog className="history-view__dialog" ref={detail} aria-labelledby={titleId} onClose={() => detailTrigger.current?.focus()} onClick={event => { if (event.target === event.currentTarget) detail.current?.close(); }}>
      <div className="history-view__top"><h2 ref={detailTitle} id={titleId} tabIndex={-1}>About your history</h2><button type="button" className="history-view__info" aria-label="Close history information" onClick={() => detail.current?.close()}>×</button></div>
      <p>Samples stay in this browser on this site. Clearing site data removes them. Export important sessions to a file.</p>
      <p>CSV includes sensor values, validity flags and missing ranges. Time is device acquisition time in microseconds, not calendar time. Optical sample timestamps are estimated.</p>
      <p>Exports stream to a file when supported. Other browsers download parts of up to 5,000 entries; continue with “Export next CSV part”.</p>
      {session && <dl><dt>Device</dt><dd>{session.deviceId}</dd><dt>Boot session</dt><dd>{session.bootId}</dd><dt>Last saved</dt><dd>{new Date(session.updatedAt).toLocaleString()}</dd></dl>}
    </dialog>
  </section>;
}
