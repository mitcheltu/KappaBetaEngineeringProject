import { CSV_COLUMNS, recordCsvValues } from './historyProtocol';
import { type HistoryEntry, type HistorySession, type HistoryStore } from './historyStore';

type CsvWriter = { write(value: string): Promise<void>; close(): Promise<void>; abort?(): Promise<void> };
type SavePicker = (options: { suggestedName: string; types: { description: string; accept: Record<string, string[]> }[] }) => Promise<{ createWritable(): Promise<CsvWriter> }>;
export type CsvExportResult = { complete: boolean; nextSequence: number; entries: number; streamed: boolean };
const cell = (value: unknown) => {
  if (value === null || value === undefined) return '';
  const text = String(value);
  return /[",\r\n]/.test(text) ? `"${text.replace(/"/g, '""')}"` : text;
};
export function historyCsvRow(session: HistorySession, entry: HistoryEntry) {
  const values: Record<string, unknown> = {
    entry: entry.type, device_id: session.deviceId, boot_session: session.bootId, sequence: entry.sequence,
    end_exclusive: entry.end, imported_at: entry.importedAt,
    ...(entry.type === 'record' ? recordCsvValues(entry.record) : {}),
  };
  return CSV_COLUMNS.map(column => cell(values[column])).join(',') + '\r\n';
}

/** Stream to a chosen file, or download explicit bounded CSV parts (5,000 rows). */
export async function exportSessionCsv(store: HistoryStore, session: HistorySession, afterSequence = -1,
  onProgress?: (entries: number) => void): Promise<CsvExportResult> {
  if (typeof window === 'undefined' || typeof document === 'undefined') throw new Error('CSV export is available in the webapp.');
  const picker = (window as Window & { showSaveFilePicker?: SavePicker }).showSaveFilePicker;
  const filename = `kappa-beta-${session.deviceId}-${session.bootId}${afterSequence >= 0 ? `-after-${afterSequence}` : ''}.csv`;
  // Keep the browser's user gesture: open the picker before any database await.
  const writer = picker ? await (await picker({ suggestedName: filename, types: [{ description: 'Sensor history CSV', accept: { 'text/csv': ['.csv'] } }] })).createWritable() : null;
  const parts: string[] = [], header = CSV_COLUMNS.join(',') + '\r\n';
  let after = afterSequence, count = 0, complete = false;
  try {
    if (writer) await writer.write(header); else parts.push(header);
    while (true) {
      const page = (await store.readEntries(session.key, after, 256)).filter(entry => entry.sequence < session.nextSequence);
      if (!page.length) { complete = true; break; }
      const selected = writer ? page : page.slice(0, 5000 - count);
      const text = selected.map(entry => historyCsvRow(session, entry)).join('');
      if (writer) await writer.write(text); else parts.push(text);
      count += selected.length; after = selected[selected.length - 1].sequence;
      onProgress?.(count);
      if (!writer && count >= 5000) {
        complete = !(await store.readEntries(session.key, after, 1)).some(entry => entry.sequence < session.nextSequence);
        break;
      }
    }
    if (writer) await writer.close();
    else {
      const url = URL.createObjectURL(new Blob(parts, { type: 'text/csv;charset=utf-8' }));
      const link = document.createElement('a'); link.href = url; link.download = filename;
      document.body.appendChild(link); link.click(); link.remove();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
    }
    return { complete, nextSequence: after, entries: count, streamed: Boolean(writer) };
  } catch (error) { await writer?.abort?.().catch(() => {}); throw error; }
}
