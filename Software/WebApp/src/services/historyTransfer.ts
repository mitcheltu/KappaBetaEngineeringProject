import { HistoryBlockAssembler, HistoryCommand as Command, HistoryStatusType as Type, MAX_CURSOR, decodeHistoryStatus,
  historyCommand, type HistoryIdentity, type HistoryStatus } from './historyProtocol';
import { type HistorySession, type HistoryStore } from './historyStore';

export type HistoryProgress = {
  phase: 'opening' | 'downloading' | 'saving' | 'live' | 'catching-up' | 'live-only' | 'error';
  sessionKey: string; deviceId: string; bootId: string; cursor: number; start: number;
  target: number | null; capacity: number | null; recordCount: number; missingCount: number; flags: number;
};
export type HistoryTransferOptions = {
  identity: HistoryIdentity; store: HistoryStore;
  write: (bytes: Uint8Array) => Promise<void>; readStatus: () => Promise<DataView>;
  onProgress: (progress: HistoryProgress) => void; onError: (error: Error) => void;
  timeoutMs?: number; pollIntervalMs?: number; now?: () => number; initialToken?: number;
};

/** One immutable block in memory. Persist -> transaction complete -> ACK. */
export function createHistoryTransfer(options: HistoryTransferOptions) {
  const now = options.now ?? Date.now;
  let active = true, started = false, live = false, pollingEnabled = false;
  let token = options.initialToken ?? Math.floor(Math.random() * 65535);
  let phase: 'opening' | 'receiving' | 'committing' | 'awaiting-live' | 'idle' = 'idle';
  let session: HistorySession | null = null;
  let cursor = 0, start = 0, target: number | null = null, capacity: number | null = null;
  let flags = options.identity.flags, assembler: HistoryBlockAssembler | undefined;
  let pendingGap: { first: number; end: number } | undefined;
  let lastBlock: HistoryBlockAssembler | undefined;
  let lastGap: { first: number; end: number } | undefined;
  let lastProgress = now(), lastProbe = now(), probePending = false, liveWritePending = false;
  let watchdog: ReturnType<typeof setInterval> | undefined, pollTimer: ReturnType<typeof setTimeout> | undefined;
  let resolveInitial!: () => void, rejectInitial!: (error: Error) => void;
  const initial = new Promise<void>((resolve, reject) => { resolveInitial = resolve; rejectInitial = reject; });
  // A disconnect may reject before the connect function reaches its await.
  void initial.catch(() => {});

  function progress(override?: HistoryProgress['phase']) {
    options.onProgress({ phase: override ?? (live ? 'catching-up' : phase === 'committing' ? 'saving' : 'downloading'),
      sessionKey: options.identity.key, deviceId: options.identity.deviceId, bootId: options.identity.bootId,
      cursor, start, target, capacity, recordCount: session?.recordCount ?? 0, missingCount: session?.missingCount ?? 0, flags });
  }
  function clearTimers() {
    if (watchdog !== undefined) clearInterval(watchdog);
    if (pollTimer !== undefined) clearTimeout(pollTimer);
    watchdog = undefined; pollTimer = undefined;
  }
  function fail(error: unknown) {
    if (!active) return;
    active = false; clearTimers();
    const problem = error instanceof Error ? error : new Error(String(error));
    progress('error');
    // Never ACK failed/corrupt/uncommitted data or silently move into live mode.
    void options.write(historyCommand(Command.ABORT, token || 1, cursor)).catch(() => {});
    if (!live) rejectInitial(problem); else options.onError(problem);
  }
  function send(opcode: number, next = cursor) {
    if (!active) return Promise.resolve();
    return options.write(historyCommand(opcode, token, next)).catch(fail);
  }
  function adoptTarget(end: number) {
    if (target !== null && target !== end) throw new Error('History snapshot boundary changed mid-transfer.');
    if (end < cursor) throw new Error('Device history is behind the saved cursor. Saved history was retained.');
    target = end;
  }
  function schedulePoll() {
    if (!active || !pollingEnabled || !live || capacity === 0 || !(flags & 1) || ((flags & 4) && cursor === MAX_CURSOR)) return;
    if (pollTimer !== undefined) clearTimeout(pollTimer);
    pollTimer = setTimeout(() => { pollTimer = undefined; begin(Command.POLL); }, options.pollIntervalMs ?? 250);
  }
  function begin(opcode: number) {
    if (!active || phase !== 'idle') return;
    token = token % 65535 + 1;
    start = cursor; target = null; assembler = undefined; pendingGap = undefined; lastBlock = undefined; lastGap = undefined;
    phase = 'opening'; lastProgress = lastProbe = now();
    progress(live ? 'catching-up' : 'opening');
    void send(opcode);
  }
  function requestLive() {
    if (liveWritePending || !active) return;
    liveWritePending = true;
    void send(Command.LIVE).finally(() => { liveWritePending = false; });
  }
  function afterCommit(saved: HistorySession, expectedToken: number, end: number) {
    if (!active || token !== expectedToken) return;
    if (saved.nextSequence !== end) { fail(new Error('Local history checkpoint changed unexpectedly. Close other connected tabs and reconnect.')); return; }
    session = saved; cursor = saved.nextSequence;
    lastBlock = assembler; lastGap = pendingGap;
    phase = 'receiving'; assembler = undefined; pendingGap = undefined;
    lastProgress = now(); progress();
    void send(Command.ACK, end);
  }
  function handleStatus(view: DataView) {
    if (!active || !started) return;
    try {
      const status = decodeHistoryStatus(view);
      if (status.token !== token) return;
      flags = status.flags;
      switch (status.type) {
        case Type.READY:
          if (phase === 'idle') return;
          if (status.first > status.end || status.count !== 0) throw new Error('Invalid READY history bounds.');
          adoptTarget(status.end);
          capacity = status.detail;
          if (capacity === 0 && status.end !== 0) throw new Error('Invalid live-only history boundary.');
          if (phase === 'opening') { phase = 'receiving'; lastProgress = now(); }
          progress();
          break;
        case Type.BLOCK: {
          if (phase === 'awaiting-live' || phase === 'idle') return;
          adoptTarget(status.end);
          if (status.first + status.count < cursor) return; // An older retry already superseded by a later ACK.
          if (lastBlock && status.first === lastBlock.status.first) {
            if (!lastBlock.matches(status)) throw new Error('Retried history block changed after saving.');
            // A queued ACK can race an old BLOCK retry. Do not let the old block
            // occupy the assembler and reject the next, already advancing block.
            void send(Command.ACK, status.first + status.count);
            return;
          }
          if (status.first > cursor || (status.first < cursor && status.first + status.count !== cursor))
            throw new Error('History block skips the durable cursor without an explicit gap.');
          if (pendingGap) throw new Error('History block arrived before its gap was committed.');
          if (assembler) {
            if (!assembler.matches(status)) throw new Error('History block changed before acknowledgement.');
          } else { assembler = new HistoryBlockAssembler(status); phase = 'receiving'; lastProgress = now(); }
          progress();
          break;
        }
        case Type.GAP: {
          if (phase === 'awaiting-live' || phase === 'idle' || status.end < cursor) return;
          if (status.count !== 0 || status.end <= status.first || (target !== null && status.end > target)) throw new Error('Invalid history gap bounds.');
          if (status.first > cursor || (status.first < cursor && status.end !== cursor)) throw new Error('History gap skips the durable cursor.');
          if (lastGap && status.first === lastGap.first && status.end === lastGap.end) {
            void send(Command.ACK, status.end); return;
          }
          if (assembler) throw new Error('History gap arrived before its block was committed.');
          if (pendingGap) {
            if (pendingGap.first !== status.first || pendingGap.end !== status.end) throw new Error('History gap changed before acknowledgement.');
            return;
          }
          pendingGap = { first: status.first, end: status.end }; phase = 'committing'; lastProgress = now(); progress();
          const expectedToken = token;
          void options.store.commitGap(options.identity, status.first, status.end)
            .then(saved => afterCommit(saved, expectedToken, status.end)).catch(fail);
          break;
        }
        case Type.END:
          if (status.first !== status.end || status.end !== cursor || status.count !== 0 || phase === 'committing')
            throw new Error('History ended before all records or gaps were saved.');
          if (phase === 'idle') return;
          adoptTarget(status.end);
          if (phase !== 'awaiting-live') lastProgress = now();
          phase = 'awaiting-live'; requestLive();
          break;
        case Type.LIVE:
          if (phase === 'idle') return; // Idempotent retry of the accepted LIVE response.
          if (phase !== 'awaiting-live' || status.first !== cursor || status.end < cursor || status.count !== 0)
            throw new Error('Unexpected history-to-live boundary.');
          capacity = status.detail;
          phase = 'idle'; assembler = undefined; lastProgress = now();
          if (!live) { live = true; resolveInitial(); }
          progress(capacity === 0 || !(flags & 1) ? 'live-only' : 'live');
          schedulePoll();
          break;
        case Type.ERROR:
          throw new Error(status.first === 1 ? 'Wearable rejected the saved history cursor. Saved records were retained.'
            : status.first === 2 ? 'History transfer timed out. Reconnect to resume saved progress.'
              : 'Wearable rejected a history command. Reconnect to retry.');
      }
    } catch (error) { fail(error); }
  }
  function handleData(view: DataView) {
    if (!active || view.byteLength < 8 || view.getUint16(0, true) !== token) return;
    try {
      if (lastBlock && view.getUint32(4, true) === lastBlock.status.first) {
        lastBlock.accept(view); return; // Verify any late retry bytes without blocking the next block.
      }
      if (!assembler || view.getUint32(4, true) !== assembler.status.first) return;
      const { complete, added } = assembler.accept(view);
      if (added) lastProgress = now();
      if (!complete || phase === 'committing') return;
      const block = assembler, expectedToken = token;
      phase = 'committing'; progress();
      void options.store.commitBlock(options.identity, block.status.first, block.bytes.slice())
        .then(saved => afterCommit(saved, expectedToken, block.status.first + block.status.count)).catch(fail);
    } catch (error) { fail(error); }
  }
  function tick() {
    if (!active || phase === 'idle') return;
    const elapsed = now() - lastProgress;
    if (elapsed >= (options.timeoutMs ?? 20000)) { fail(new Error('History stopped responding. Reconnect to resume the durable checkpoint.')); return; }
    if (phase === 'committing' || probePending || now() - lastProbe < 3000) return;
    lastProbe = now(); probePending = true;
    if (phase === 'awaiting-live') requestLive();
    void options.readStatus().then(view => { if (active) handleStatus(view); }).catch(fail).finally(() => { probePending = false; });
  }
  return {
    async start() {
      if (started) return initial;
      started = true;
      try {
        if (options.identity.flags & 1) session = await options.store.ensureSession(options.identity);
        if (!active) return initial;
        cursor = session?.nextSequence ?? 0;
        watchdog = setInterval(tick, 500);
        begin(Command.OPEN);
      } catch (error) { fail(error); }
      return initial;
    },
    handleStatus, handleData,
    enablePolling() { pollingEnabled = true; if (phase === 'idle') schedulePoll(); },
    tick,
    cancel() {
      if (!active) return;
      active = false; clearTimers(); assembler = undefined;
      if (!live) rejectInitial(new Error('History connection cancelled.'));
    },
  };
}
