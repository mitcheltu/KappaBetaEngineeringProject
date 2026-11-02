import assert from 'node:assert/strict';
import test from 'node:test';
import {
  createMockConnectionService,
  DEMO_DEVICE_NAME,
  type ConnectionStatus,
  type MockConnectionService,
} from './mockConnection';

const delay = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

function waitFor(service: MockConnectionService, status: ConnectionStatus) {
  if (service.getSnapshot().status === status) return Promise.resolve();
  return new Promise<void>((resolve, reject) => {
    const timeout = setTimeout(() => {
      unsubscribe();
      reject(new Error(`Expected ${status}; got ${service.getSnapshot().status}`));
    }, 500);
    const unsubscribe = service.subscribe(() => {
      if (service.getSnapshot().status === status) {
        clearTimeout(timeout);
        unsubscribe();
        resolve();
      }
    });
  });
}

test('normal discovery connects only after selecting the fictional device', async (t) => {
  const service = createMockConnectionService({ delayMs: 5 });
  t.after(() => service.dispose());
  assert.deepEqual(service.getSnapshot(), {
    status: 'disconnected', deviceName: null, scenario: 'normal',
  });

  service.connect();
  assert.equal(service.getSnapshot().status, 'disconnected');
  service.scan();
  assert.equal(service.getSnapshot().status, 'scanning');
  await waitFor(service, 'results');
  assert.equal(service.getSnapshot().deviceName, DEMO_DEVICE_NAME);
  service.connect();
  assert.equal(service.getSnapshot().status, 'connecting');
  await waitFor(service, 'connected');
  service.disconnect();
  assert.equal(service.getSnapshot().status, 'disconnected');
  assert.equal(service.getSnapshot().deviceName, null);
});

test('closing during scanning or connecting prevents late completion', async (t) => {
  const service = createMockConnectionService({ delayMs: 5 });
  t.after(() => service.dispose());
  service.scan();
  service.close();
  await delay(20);
  assert.equal(service.getSnapshot().status, 'disconnected');

  service.scan();
  await waitFor(service, 'results');
  service.connect();
  service.close();
  await delay(20);
  assert.equal(service.getSnapshot().status, 'disconnected');
  assert.equal(service.getSnapshot().deviceName, null);
});

test('duplicate requests and scenario changes cannot replace pending work', async (t) => {
  const service = createMockConnectionService({ delayMs: 5 });
  t.after(() => service.dispose());
  service.scan();
  const scanning = service.getSnapshot();
  service.scan();
  service.connect();
  service.setScenario('empty');
  assert.equal(service.getSnapshot(), scanning);
  await waitFor(service, 'results');
  service.connect();
  const connecting = service.getSnapshot();
  service.connect();
  service.scan();
  service.chooseDevice();
  service.setScenario('failure');
  assert.equal(service.getSnapshot(), connecting);
  await waitFor(service, 'connected');
});

test('empty results can be retried after choosing the normal scenario', async (t) => {
  const service = createMockConnectionService({ delayMs: 5 });
  t.after(() => service.dispose());
  service.setScenario('empty');
  service.scan();
  await waitFor(service, 'empty');
  assert.equal(service.getSnapshot().deviceName, null);
  service.setScenario('normal');
  service.scan();
  await waitFor(service, 'results');
});

test('failure supports retry and returning to the existing device results', async (t) => {
  const service = createMockConnectionService({ delayMs: 5 });
  t.after(() => service.dispose());
  service.setScenario('failure');
  service.scan();
  await waitFor(service, 'results');
  service.connect();
  await waitFor(service, 'error');
  service.connect();
  await waitFor(service, 'error');
  service.chooseDevice();
  assert.equal(service.getSnapshot().status, 'results');
  assert.equal(service.getSnapshot().deviceName, DEMO_DEVICE_NAME);
  service.setScenario('normal');
  service.connect();
  await waitFor(service, 'connected');
});

test('connection loss supports a fresh discovery or dismissal', async (t) => {
  const service = createMockConnectionService({ delayMs: 5 });
  t.after(() => service.dispose());
  service.loseConnection();
  assert.equal(service.getSnapshot().status, 'disconnected');
  service.scan();
  await waitFor(service, 'results');
  service.connect();
  await waitFor(service, 'connected');
  service.loseConnection();
  assert.equal(service.getSnapshot().status, 'lost');
  service.scan();
  assert.equal(service.getSnapshot().deviceName, null);
  await waitFor(service, 'results');
  service.connect();
  await waitFor(service, 'connected');
  service.loseConnection();
  service.close();
  assert.equal(service.getSnapshot().status, 'disconnected');
});

test('snapshots stay stable until a change and subscriptions can unsubscribe', (t) => {
  const service = createMockConnectionService();
  t.after(() => service.dispose());
  const initial = service.getSnapshot();
  let changes = 0;
  const unsubscribe = service.subscribe(() => { changes += 1; });
  assert.equal(service.getSnapshot(), initial);
  service.close();
  service.setScenario('normal');
  assert.equal(service.getSnapshot(), initial);
  assert.equal(changes, 0);
  service.setScenario('empty');
  assert.notEqual(service.getSnapshot(), initial);
  assert.equal(changes, 1);
  unsubscribe();
  service.setScenario('failure');
  assert.equal(changes, 1);
});

test('disposal cancels pending operations and suppresses further updates', async () => {
  const service = createMockConnectionService({ delayMs: 5 });
  service.scan();
  let changes = 0;
  service.subscribe(() => { changes += 1; });
  const beforeDispose = service.getSnapshot();
  service.dispose();
  await delay(20);
  service.scan();
  service.connect();
  service.close();
  service.setScenario('failure');
  assert.equal(changes, 0);
  assert.equal(service.getSnapshot(), beforeDispose);
});

test('a subscriber can synchronously cancel the operation being announced', async (t) => {
  const service = createMockConnectionService({ delayMs: 5 });
  t.after(() => service.dispose());
  service.subscribe(() => {
    if (service.getSnapshot().status === 'scanning') service.close();
  });
  service.scan();
  await delay(20);
  assert.equal(service.getSnapshot().status, 'disconnected');
});
