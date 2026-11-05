import { crc32, decodeHistoryBlock, type HistoryIdentity, type HistoryStatus } from './historyProtocol';
import { newSession, type HistoryEntry, type HistorySession, type HistoryStore } from './historyStore';

export const identity: HistoryIdentity = { key: '001122334455:0102030405060708', deviceId: '001122334455', bootId: '0102030405060708', flags: 3 };
export const flush = () => new Promise<void>(resolve => setImmediate(resolve));
export function status(type: number, token = 1, first = 0, end = first, detail = 0, count = 0, flags = 3) {
  const view = new DataView(new ArrayBuffer(20)); view.setUint8(0, type); view.setUint8(1, 1);
  view.setUint16(2, token, true); view.setUint32(4, first, true); view.setUint32(8, end, true);
  view.setUint32(12, detail, true); view.setUint16(16, count, true); view.setUint16(18, flags, true); return view;
}
export function records(first = 0, count = 2, kind = 2) {
  const bytes = new Uint8Array(count * 32), view = new DataView(bytes.buffer);
  for (let i = 0; i < count; i++) {
    view.setUint32(i * 32, first + i, true); view.setUint8(i * 32 + 4, kind); view.setUint8(i * 32 + 5, 1);
    view.setBigUint64(i * 32 + 8, 10000000000000000n + BigInt(i), true); view.setInt16(i * 32 + 16, 1000, true);
  }
  return bytes;
}
export function fragment(bytes: Uint8Array, token = 1, first = 0, offset = 0, count = bytes.length - offset) {
  const out = new Uint8Array(8 + count), view = new DataView(out.buffer);
  view.setUint16(0, token, true); view.setUint16(2, offset, true); view.setUint32(4, first, true); out.set(bytes.slice(offset, offset + count), 8); return view;
}
export function blockStatus(bytes: Uint8Array, token = 1, first = 0, target = first + bytes.length / 32) {
  return status(2, token, first, target, crc32(bytes), bytes.length / 32);
}
export function command(bytes: Uint8Array) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  return { opcode: bytes[0], token: view.getUint16(2, true), cursor: view.getUint32(4, true) };
}
export class MemoryHistoryStore implements HistoryStore {
  sessions = new Map<string, HistorySession>(); entries = new Map<string, HistoryEntry[]>();
  async ensureSession(id: HistoryIdentity) { const session = this.sessions.get(id.key) ?? newSession(id); this.sessions.set(id.key, session); return { ...session }; }
  async commitBlock(id: HistoryIdentity, first: number, bytes: Uint8Array) {
    const session = await this.ensureSession(id), decoded = decodeHistoryBlock(bytes, first), rows = this.entries.get(id.key) ?? [];
    if (first !== session.nextSequence) {
      if (first + decoded.length > session.nextSequence) throw new Error('Cursor mismatch');
      for (const record of decoded) {
        const saved = rows.find(row => row.sequence === record.sequence);
        if (!saved || saved.type !== 'record' || String(new Uint8Array(saved.record.raw)) !== String(new Uint8Array(record.raw))) throw new Error('Conflicting retry');
      }
      return session;
    }
    rows.push(...decoded.map(record => ({ type: 'record' as const, sessionKey: id.key, sequence: record.sequence, end: record.sequence + 1, importedAt: new Date().toISOString(), record })));
    this.entries.set(id.key, rows);
    const saved = { ...session, nextSequence: first + decoded.length, recordCount: session.recordCount + decoded.length };
    this.sessions.set(id.key, saved); return { ...saved };
  }
  async commitGap(id: HistoryIdentity, first: number, end: number) {
    const session = await this.ensureSession(id), rows = this.entries.get(id.key) ?? [];
    if (first !== session.nextSequence) {
      if (!rows.some(row => row.type === 'gap' && row.sequence === first && row.end === end)) throw new Error('Conflicting gap');
      return session;
    }
    rows.push({ type: 'gap', sessionKey: id.key, sequence: first, end, importedAt: new Date().toISOString() }); this.entries.set(id.key, rows);
    const saved = { ...session, nextSequence: end, missingCount: session.missingCount + end - first }; this.sessions.set(id.key, saved); return { ...saved };
  }
  async listSessions() { return [...this.sessions.values()]; }
  async readEntries(key: string, after: number, limit: number) { return (this.entries.get(key) ?? []).filter(row => row.sequence > after).slice(0, limit); }
}
