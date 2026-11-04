export type Scenario = 'normal' | 'empty' | 'failure';

export type ConnectionStatus =
  | 'disconnected'
  | 'scanning'
  | 'results'
  | 'connecting'
  | 'connected'
  | 'empty'
  | 'error'
  | 'lost';

export type ConnectionSnapshot = Readonly<{
  status: ConnectionStatus;
  deviceName: string | null;
  scenario: Scenario;
}>;

export const DEMO_DEVICE_NAME = 'Demo Wearable ESP32';

export type MockConnectionService = ReturnType<typeof createMockConnectionService>;

/** Local-only simulation. No Bluetooth, network, storage, or sensor APIs. */
export function createMockConnectionService(
  options: { delayMs?: number; deviceName?: string } = {},
) {
  const delayMs = Math.max(0, options.delayMs ?? 1000);
  const deviceName = options.deviceName ?? DEMO_DEVICE_NAME;
  const listeners = new Set<() => void>();
  let snapshot: ConnectionSnapshot = Object.freeze({
    status: 'disconnected',
    deviceName: null,
    scenario: 'normal',
  });
  let timer: ReturnType<typeof setTimeout> | undefined;
  let generation = 0;
  let disposed = false;

  const pending = () =>
    snapshot.status === 'scanning' || snapshot.status === 'connecting';

  function publish(next: ConnectionSnapshot) {
    if (
      disposed ||
      (snapshot.status === next.status &&
        snapshot.deviceName === next.deviceName &&
        snapshot.scenario === next.scenario)
    ) {
      return;
    }
    snapshot = Object.freeze(next);
    // Copy before notifying so a listener can unsubscribe during notification.
    for (const listener of [...listeners]) listener();
  }

  function cancelPending() {
    generation += 1;
    if (timer !== undefined) clearTimeout(timer);
    timer = undefined;
  }

  function begin(
    status: 'scanning' | 'connecting',
    outcome: ConnectionStatus,
  ) {
    cancelPending();
    const operationGeneration = generation;
    // Install the timer before notifying: synchronous cancellation from a
    // subscriber must also cancel this operation.
    timer = setTimeout(() => {
      if (disposed || operationGeneration !== generation) return;
      timer = undefined;
      publish({
        ...snapshot,
        status: outcome,
        deviceName: outcome === 'empty' ? null : deviceName,
      });
    }, delayMs);
    publish({
      ...snapshot,
      status,
      deviceName: status === 'scanning' ? null : deviceName,
    });
  }

  function close() {
    if (disposed) return;
    cancelPending();
    publish({ ...snapshot, status: 'disconnected', deviceName: null });
  }

  return {
    getSnapshot: () => snapshot,

    subscribe(listener: () => void) {
      if (disposed) return () => {};
      listeners.add(listener);
      return () => {
        listeners.delete(listener);
      };
    },

    scan() {
      if (disposed || pending() || snapshot.status === 'connected') return;
      begin('scanning', snapshot.scenario === 'empty' ? 'empty' : 'results');
    },

    connect() {
      if (
        disposed ||
        (snapshot.status !== 'results' && snapshot.status !== 'error')
      ) {
        return;
      }
      begin('connecting', snapshot.scenario === 'failure' ? 'error' : 'connected');
    },

    chooseDevice() {
      if (disposed || snapshot.status !== 'error') return;
      publish({ ...snapshot, status: 'results', deviceName });
    },

    close,
    disconnect: close,

    loseConnection() {
      if (disposed || snapshot.status !== 'connected') return;
      publish({ ...snapshot, status: 'lost' });
    },

    setScenario(scenario: Scenario) {
      // A pending attempt always completes according to the scenario it began
      // with. The UI can change scenarios once that attempt finishes or closes.
      if (disposed || pending()) return;
      publish({ ...snapshot, scenario });
    },

    dispose() {
      if (disposed) return;
      cancelPending();
      disposed = true;
      listeners.clear();
    },
  };
}
