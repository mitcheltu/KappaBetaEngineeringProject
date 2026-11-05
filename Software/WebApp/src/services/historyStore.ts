import { decodeHistoryBlock, type HistoryIdentity, type HistoryRecord } from './historyProtocol';

export type HistorySession = HistoryIdentity & { nextSequence: number; recordCount: number; missingCount: number; createdAt: string; updatedAt: string; lastTimeUs: string | null };
export type HistoryEntry = { sessionKey: string; sequence: number; end: number; importedAt: string } &
  ({ type: 'record'; record: HistoryRecord } | { type: 'gap' });
export interface HistoryStore {
  ensureSession(identity: HistoryIdentity): Promise<HistorySession>;
  commitBlock(identity: HistoryIdentity, first: number, bytes: Uint8Array): Promise<HistorySession>;
  commitGap(identity: HistoryIdentity, first: number, end: number): Promise<HistorySession>;
  listSessions(): Promise<HistorySession[]>;
  readEntries(sessionKey: string, afterSequence: number, limit: number): Promise<HistoryEntry[]>;
}
export function newSession(identity: HistoryIdentity): HistorySession {
  const date = new Date().toISOString();
  return { ...identity, nextSequence: 0, recordCount: 0, missingCount: 0, createdAt: date, updatedAt: date, lastTimeUs: null };
}
function sameBytes(a: ArrayBuffer, b: ArrayBuffer) {
  const x = new Uint8Array(a), y = new Uint8Array(b);
  return x.length === y.length && x.every((value, index) => value === y[index]);
}
function storageError(error: unknown): Error {
  if (error instanceof Error && error.name === 'QuotaExceededError') return new Error('Browser storage is full. History was not acknowledged; free browser storage and reconnect.');
  return error instanceof Error ? error : new Error('Local history storage failed.');
}

export function createIndexedDbHistoryStore(databaseName = 'kappa-beta-sensor-history'): HistoryStore {
  let database: Promise<IDBDatabase> | undefined;
  function open() {
    if (!database) database = new Promise<IDBDatabase>((resolve, reject) => {
      if (typeof indexedDB === 'undefined') { reject(new Error('IndexedDB is unavailable. A browser with local storage is required to save sensor history.')); return; }
      const request = indexedDB.open(databaseName, 1);
      let abandoned = false;
      request.onupgradeneeded = () => {
        const db = request.result;
        db.createObjectStore('sessions', { keyPath: 'key' });
        db.createObjectStore('entries', { keyPath: ['sessionKey', 'sequence'] });
      };
      request.onerror = () => { database = undefined; reject(storageError(request.error)); };
      request.onblocked = () => { abandoned = true; database = undefined; reject(new Error('Close other Kappa Beta tabs to open the history database.')); };
      request.onsuccess = () => {
        const db = request.result;
        if (abandoned) { db.close(); return; }
        db.onversionchange = () => { db.close(); database = undefined; };
        resolve(db);
      };
    });
    return database;
  }

  async function change(identity: HistoryIdentity, apply: (session: HistorySession, entries: IDBObjectStore, save: (next: HistorySession) => void, fail: (error: Error) => void) => void) {
    const db = await open();
    return new Promise<HistorySession>((resolve, reject) => {
      // A request success is insufficient: ACK callers only see transaction completion.
      const tx = db.transaction(['sessions', 'entries'], 'readwrite', { durability: 'strict' });
      let result: HistorySession | undefined, failure: Error | undefined;
      const fail = (error: Error) => { if (failure) return; failure = error; tx.abort(); };
      const sessions = tx.objectStore('sessions'), entries = tx.objectStore('entries');
      tx.oncomplete = () => result ? resolve(result) : reject(new Error('History transaction completed without a checkpoint.'));
      tx.onabort = () => reject(failure ?? storageError(tx.error));
      tx.onerror = () => { /* Abort is the final transaction result. */ };
      const request = sessions.get(identity.key);
      request.onsuccess = () => {
        try {
          const current: HistorySession = request.result ?? newSession(identity);
          apply(current, entries, next => { result = next; sessions.put(next); }, fail);
        } catch (error) { fail(storageError(error)); }
      };
    });
  }

  return {
    ensureSession: identity => change(identity, (session, _entries, save) => save({ ...session, flags: identity.flags })),
    commitBlock(identity, first, bytes) {
      const records = decodeHistoryBlock(bytes, first), end = first + records.length;
      return change(identity, (session, entries, save, fail) => {
        if (first !== session.nextSequence) {
          if (end > session.nextSequence) { fail(new Error('History storage cursor mismatch; refusing an unreported gap.')); return; }
          // A retry after a lost ACK must match the already saved raw bytes.
          let remaining = records.length;
          for (const record of records) {
            const request = entries.get([identity.key, record.sequence]);
            request.onsuccess = () => {
              const saved = request.result as HistoryEntry | undefined;
              if (!saved || saved.type !== 'record' || !sameBytes(saved.record.raw, record.raw)) {
                fail(new Error('Retried history conflicts with saved records.')); return;
              }
              if (--remaining === 0) save(session);
            };
          }
          return;
        }
        const importedAt = new Date().toISOString();
        for (const record of records) entries.put({ sessionKey: identity.key, sequence: record.sequence, end: record.sequence + 1, importedAt, type: 'record', record } satisfies HistoryEntry);
        save({ ...session, flags: identity.flags, nextSequence: end, recordCount: session.recordCount + records.length,
          updatedAt: importedAt, lastTimeUs: records[records.length - 1].timeUs });
      });
    },
    commitGap(identity, first, end) {
      if (!Number.isInteger(first) || !Number.isInteger(end) || first < 0 || end <= first || end > 0xffffffff)
        return Promise.reject(new Error('Invalid history gap.'));
      return change(identity, (session, entries, save, fail) => {
        if (first !== session.nextSequence) {
          if (end > session.nextSequence) { fail(new Error('History gap does not start at the durable cursor.')); return; }
          const request = entries.get([identity.key, first]);
          request.onsuccess = () => {
            const saved = request.result as HistoryEntry | undefined;
            if (!saved || saved.type !== 'gap' || saved.end !== end) { fail(new Error('Retried history gap conflicts with saved data.')); return; }
            save(session);
          };
          return;
        }
        const importedAt = new Date().toISOString();
        entries.put({ sessionKey: identity.key, sequence: first, end, importedAt, type: 'gap' } satisfies HistoryEntry);
        save({ ...session, flags: identity.flags, nextSequence: end, missingCount: session.missingCount + end - first, updatedAt: importedAt });
      });
    },
    async listSessions() {
      const db = await open();
      return new Promise<HistorySession[]>((resolve, reject) => {
        const tx = db.transaction('sessions', 'readonly'), request = tx.objectStore('sessions').getAll();
        tx.oncomplete = () => resolve((request.result as HistorySession[]).sort((a, b) => b.updatedAt.localeCompare(a.updatedAt)));
        tx.onabort = () => reject(storageError(tx.error));
      });
    },
    async readEntries(sessionKey, afterSequence, limit) {
      const db = await open();
      if (!Number.isInteger(limit) || limit < 1 || limit > 1024) throw new Error('History page size must be 1–1024.');
      return new Promise<HistoryEntry[]>((resolve, reject) => {
        const tx = db.transaction('entries', 'readonly'), entries: HistoryEntry[] = [];
        const range = IDBKeyRange.bound([sessionKey, afterSequence], [sessionKey, 0xffffffff], true, false);
        const request = tx.objectStore('entries').openCursor(range);
        request.onsuccess = () => {
          const cursor = request.result;
          if (!cursor) return;
          entries.push(cursor.value as HistoryEntry);
          if (entries.length < limit) cursor.continue();
        };
        tx.oncomplete = () => resolve(entries);
        tx.onabort = () => reject(storageError(tx.error));
      });
    },
  };
}
