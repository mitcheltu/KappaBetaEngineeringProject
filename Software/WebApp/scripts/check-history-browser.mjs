// Run after the web build. Uses an existing Playwright installation and Chromium.
// node scripts/check-history-browser.mjs <playwright/index.mjs> <chromium.exe>
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { mkdir, readFile } from 'node:fs/promises';
import { dirname, extname, resolve, sep } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { build } from 'esbuild';

const app = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const { chromium } = await import(process.argv[2] ? pathToFileURL(resolve(process.argv[2])).href : 'playwright');
const bundle = await build({ stdin: { contents: `export * from './src/services/historyStore'; export * from './src/services/historyProtocol'; export * from './src/services/historyExport';`, resolveDir: app, loader: 'ts' }, bundle: true, write: false, format: 'iife', globalName: 'HistoryTestApi', platform: 'browser' });
const api = bundle.outputFiles[0].text;
const root = resolve(app, process.argv[4] || 'dist'), output = resolve(app, '.expo/history-check');
await mkdir(output, { recursive: true });
const types = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.png': 'image/png', '.woff2': 'font/woff2' };
const server = createServer(async (req, res) => {
  const url = new URL(req.url, 'http://localhost');
  if (url.pathname === '/database-test.html') { res.writeHead(200, { 'Content-Type': 'text/html' }); res.end('<!doctype html><title>History database checks</title>'); return; }
  const path = resolve(root, '.' + url.pathname);
  if (path !== root && !path.startsWith(root + sep)) { res.writeHead(403); res.end(); return; }
  const file = path === root ? resolve(root, 'index.html') : path;
  try { const body = await readFile(file); res.writeHead(200, { 'Content-Type': types[extname(file)] || 'application/octet-stream' }); res.end(body); }
  catch { res.writeHead(404); res.end(); }
});
await new Promise(done => server.listen(0, '127.0.0.1', done));
const browser = await chromium.launch({ headless: true, ...(process.argv[3] ? { executablePath: resolve(process.argv[3]) } : {}) });
const url = `http://127.0.0.1:${server.address().port}`;
try {
  const page = await browser.newPage();
  await page.goto(`${url}/database-test.html`); await page.addScriptTag({ content: api });
  const result = await page.evaluate(async () => {
    const { createIndexedDbHistoryStore, decodeHistoryBlock, historyCsvRow, exportSessionCsv } = HistoryTestApi;
    const check = (value, message) => { if (!value) throw new Error(message); };
    const id = { key: 'device:boot', deviceId: 'device', bootId: 'boot', flags: 3 };
    const bytes = (first, count) => {
      const result = new Uint8Array(count * 32), view = new DataView(result.buffer);
      for (let i = 0; i < count; i++) { view.setUint32(i * 32, first + i, true); view.setUint8(i * 32 + 4, 2); view.setUint8(i * 32 + 5, 1); view.setBigUint64(i * 32 + 8, BigInt(1000000 + i), true); }
      return result;
    };
    const store = createIndexedDbHistoryStore('history-real-idb-check');
    const events = [], originalTransaction = IDBDatabase.prototype.transaction;
    IDBDatabase.prototype.transaction = function(...args) {
      const tx = originalTransaction.apply(this, args);
      if (args[1] === 'readwrite') { check(args[2]?.durability === 'strict', 'durability not strict'); tx.addEventListener('complete', () => events.push('complete')); }
      return tx;
    };
    await store.ensureSession(id);
    const first = bytes(0, 2);
    await store.commitBlock(id, 0, first).then(() => events.push('resolved'));
    check(events.at(-2) === 'complete' && events.at(-1) === 'resolved', 'resolved before transaction completion');
    check((await store.commitBlock(id, 0, first)).recordCount === 2, 'retry duplicated records');
    const changed = first.slice(); changed[16] = 99;
    let rejected = false; try { await store.commitBlock(id, 0, changed); } catch { rejected = true; }
    check(rejected, 'changed duplicate accepted');
    const originalPut = IDBObjectStore.prototype.put; let abortNext = true;
    IDBObjectStore.prototype.put = function(...args) {
      const request = originalPut.apply(this, args);
      if (this.name === 'entries' && abortNext) { abortNext = false; const tx = this.transaction; request.addEventListener('success', () => tx.abort()); }
      return request;
    };
    rejected = false; try { await store.commitBlock(id, 2, bytes(2, 2)); } catch { rejected = true; }
    IDBObjectStore.prototype.put = originalPut;
    check(rejected, 'transaction abort accepted');
    check((await store.ensureSession(id)).nextSequence === 2, 'abort advanced checkpoint');
    check((await store.readEntries(id.key, -1, 100)).length === 2, 'abort retained partial records');
    await store.commitGap(id, 2, 8); await store.commitGap(id, 2, 8);
    const second = createIndexedDbHistoryStore('history-real-idb-check');
    await Promise.all([store.commitBlock(id, 8, bytes(8, 2)), second.commitBlock(id, 8, bytes(8, 2))]);
    const session = await second.ensureSession(id);
    check(session.nextSequence === 10 && session.recordCount === 4 && session.missingCount === 6, 'atomic concurrent retry count');
    const entries = await store.readEntries(id.key, -1, 2);
    check(entries.length === 2 && (await store.readEntries(id.key, entries[1].sequence, 2))[0].type === 'gap', 'bounded paging gap missing');
    check(historyCsvRow(session, (await store.readEntries(id.key, 1, 1))[0]).startsWith('gap,device,boot,2,8,'), 'gap CSV');
    const fakeSession = { ...session, nextSequence: 5001, recordCount: 5001 };
    let maxPage = 0, pickerOpened = false, total = 0, maxWrite = 0;
    const exportStore = { readEntries: async (_key, after, limit) => {
      maxPage = Math.max(maxPage, limit);
      return Array.from({ length: Math.max(0, Math.min(limit, 5000 - after)) }, (_, index) => {
        const seq = after + 1 + index; return { sessionKey: id.key, sequence: seq, end: seq + 1, importedAt: '', type: 'record', record: decodeHistoryBlock(bytes(seq, 1), seq)[0] };
      });
    } };
    Object.defineProperty(window, 'showSaveFilePicker', { configurable: true, value: async () => {
      pickerOpened = true;
      return { createWritable: async () => ({ write: async text => { check(pickerOpened, 'picker gesture order'); total += text.split('\r\n').length - 1; maxWrite = Math.max(maxWrite, text.split('\r\n').length - 1); }, close: async () => {} }) };
    } });
    const streamed = await exportSessionCsv(exportStore, fakeSession);
    check(streamed.complete && streamed.entries === 5001 && total === 5002 && maxWrite <= 256 && maxPage === 256, 'CSV stream not bounded/complete');
    Object.defineProperty(window, 'showSaveFilePicker', { configurable: true, value: undefined });
    const originalUrl = URL.createObjectURL, blobs = [];
    URL.createObjectURL = blob => { blobs.push(blob); return originalUrl(blob); };
    const part = await exportSessionCsv(exportStore, fakeSession);
    const tail = await exportSessionCsv(exportStore, fakeSession, part.nextSequence);
    check(!part.complete && part.entries === 5000 && tail.complete && tail.entries === 1, 'CSV parts dropped entries');
    check((await blobs[0].text()).split('\r\n').length === 5002, 'CSV first part row count');
    return { checkpoint: session.nextSequence, records: session.recordCount, gaps: session.missingCount, csvEntries: streamed.entries, abortAtomic: true, strictCompletion: true };
  });
  await page.reload(); await page.addScriptTag({ content: api });
  assert.equal(await page.evaluate(async () => (await HistoryTestApi.createIndexedDbHistoryStore('history-real-idb-check').listSessions())[0].nextSequence), 10);
  console.log('Real Chromium IndexedDB and CSV:', result);
  await page.close();

  for (const [name, width, height] of [['desktop', 1440, 1000], ['mobile', 390, 844]]) {
    const page = await browser.newPage({ viewport: { width, height } });
    const errors = []; page.on('pageerror', error => errors.push(error.message));
    const installFakeBluetooth = () => {
      const { HISTORY_UUIDS, crc32 } = HistoryTestApi, suffix = '-7e57-4d8e-b6f4-889c1d4e8270';
      const chars = {}, commands = []; let target = 6, live = false, token = 0, cursor = 0;
      const make = value => { const c = new EventTarget(); c.value = value; c.readValue = async () => c.value; c.startNotifications = async () => c; c.writeValueWithResponse = async () => {}; c.notify = value => { c.value = value; c.dispatchEvent(new Event('characteristicvaluechanged')); }; return c; };
      const status = (type, first, end, detail = 0, count = 0) => {
        const v = new DataView(new ArrayBuffer(20)); v.setUint8(0, type); v.setUint8(1, 1); v.setUint16(2, token, true); v.setUint32(4, first, true); v.setUint32(8, end, true); v.setUint32(12, detail, true); v.setUint16(16, count, true); v.setUint16(18, 3, true); chars[HISTORY_UUIDS.status].notify(v);
      };
      const next = () => {
        if (cursor < 2) { status(4, cursor, 2); return; }
        if (cursor >= target) { status(3, target, target); return; }
        const bytes = new Uint8Array((target - cursor) * 32), v = new DataView(bytes.buffer);
        for (let i = 0; i < target - cursor; i++) { v.setUint32(i * 32, cursor + i, true); v.setUint8(i * 32 + 4, 2); v.setUint8(i * 32 + 5, 1); v.setBigUint64(i * 32 + 8, BigInt(1000000 + (cursor + i) * 20000), true); v.setInt16(i * 32 + 16, 1000, true); }
        status(2, cursor, target, crc32(bytes), target - cursor);
        for (let offset = 0; offset < bytes.length; offset += 12) { const out = new Uint8Array(8 + Math.min(12, bytes.length - offset)), view = new DataView(out.buffer); view.setUint16(0, token, true); view.setUint16(2, offset, true); view.setUint32(4, cursor, true); out.set(bytes.slice(offset, offset + 12), 8); chars[HISTORY_UUIDS.data].notify(view); }
      };
      for (let index = 0; index < 4; index++) {
        const value = new DataView(new ArrayBuffer(20)); value.setUint8(0, 1); value.setUint8(1, index === 0 ? 3 : index === 2 ? 7 : 1); value.setUint16(2, 12, true); value.setUint32(4, 1234, true);
        if (index === 0) { value.setFloat32(8, 113550, true); value.setFloat32(12, 1.25, true); value.setFloat32(16, 22.5, true); }
        if (index === 2) { value.setUint32(8, 50000, true); value.setUint32(12, 60000, true); value.setUint16(16, 720, true); value.setUint16(18, 9800, true); }
        if (index === 3) value.setUint16(8, 4100, true);
        const c = make(value); c.startNotifications = async () => { if (!live) throw new Error('Live subscribed before history handshake'); return c; }; chars[`e81b621${index}${suffix}`] = c;
      }
      const id = new DataView(new ArrayBuffer(20)); id.setUint8(0, 1); id.setUint8(1, 32); id.setUint8(2, 3); id.setUint8(4, 1); id.setUint8(10, 2);
      chars[HISTORY_UUIDS.identity] = make(id); chars[HISTORY_UUIDS.status] = make(new DataView(new ArrayBuffer(20))); chars[HISTORY_UUIDS.data] = make(new DataView(new ArrayBuffer(20))); chars[HISTORY_UUIDS.command] = make(); chars[`e81b6220${suffix}`] = make();
      chars[HISTORY_UUIDS.command].writeValueWithResponse = async bytes => {
        const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength), op = bytes[0]; token = v.getUint16(2, true); cursor = v.getUint32(4, true); commands.push({ op, cursor, token });
        if (op === 1 || op === 4) { if (op === 4 && target < 8) target = 8; status(1, 2, target, 131072); next(); }
        if (op === 2) next();
        if (op === 3) { live = true; status(5, cursor, target, 131072); }
      };
      const device = new EventTarget(); device.name = 'KappaBeta-RevF'; device.gatt = { connected: false, async connect() { this.connected = true; live = false; return this; }, disconnect() { const was = this.connected; this.connected = false; if (was) device.dispatchEvent(new Event('gattserverdisconnected')); }, async getPrimaryService() { return { getCharacteristic: async uuid => chars[uuid] }; } };
      Object.defineProperty(navigator, 'bluetooth', { configurable: true, value: { requestDevice: async () => device } });
      Object.defineProperty(window, 'showSaveFilePicker', { configurable: true, value: undefined });
      globalThis.historyCommands = commands;
    };
    await page.addInitScript({ content: `${api}\n(${installFakeBluetooth.toString()})();` });
    await page.goto(url, { waitUntil: 'networkidle' });
    await page.getByRole('button', { name: 'Connect wearable', exact: true }).click();
    try { await page.getByText('CONNECTED / LIVE', { exact: true }).waitFor(); }
    catch (error) { console.log('Connection failure:', await page.locator('#live-telemetry').innerText(), errors, await page.evaluate(() => globalThis.historyCommands)); throw error; }
    await page.getByText('72 bpm', { exact: true }).waitFor();
    await page.getByText(/6 records · 2 unavailable/).waitFor();
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false, `${name} overflow`);
    await page.locator('#live-telemetry').screenshot({ path: resolve(output, `${name}-history.png`) });
    const download = page.waitForEvent('download'); await page.getByRole('button', { name: 'Export CSV', exact: true }).click();
    assert.match((await download).suggestedFilename(), /^kappa-beta-/);
    await page.getByRole('button', { name: 'Disconnect wearable', exact: true }).click();
    await page.getByRole('button', { name: 'Connect wearable', exact: true }).click();
    await page.getByText('CONNECTED / LIVE', { exact: true }).waitFor();
    assert.equal(await page.evaluate(() => historyCommands.filter(c => c.op === 1).at(-1).cursor), 8, 'reconnect did not resume checkpoint');
    assert.deepEqual(errors, []); console.log(`${name} UI: history-first, background tail, saved session, CSV, reconnect, no overflow/errors`); await page.close();
  }
} finally { await browser.close(); await new Promise(done => server.close(done)); }
