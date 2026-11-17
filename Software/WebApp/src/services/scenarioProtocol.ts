// Demo scenario BLE protocol v1 (firmware: Software/testsAndDemo/DrowningWearableDemo/ScenarioProtocol.h).
// The app may only ARM a scenario that is compiled into the wearable, or ABORT one before the
// outputs start. It never starts, stops or times the vibration motor or pump: those start only
// from on-device inference over the scripted scenario, and physical Cancel / double tap stop them.

export const SCENARIO_UUIDS = {
  catalog: 'e81b6240-7e57-4d8e-b6f4-889c1d4e8270',
  command: 'e81b6241-7e57-4d8e-b6f4-889c1d4e8270',
  status: 'e81b6242-7e57-4d8e-b6f4-889c1d4e8270',
} as const;
export const SCENARIO_VERSION = 1;
const HEADER_BYTES = 4, ENTRY_BYTES = 40, NAME_BYTES = 24, STATUS_BYTES = 20, COMMAND_BYTES = 8;

export type ScenarioState = 'idle' | 'armed' | 'playing' | 'alerted' | 'actuating' | 'done' | 'aborted' | 'rejected';
const STATES: ScenarioState[] = ['idle', 'armed', 'playing', 'alerted', 'actuating', 'done', 'aborted', 'rejected'];
export const RUNNING_STATES: readonly ScenarioState[] = ['armed', 'playing', 'alerted', 'actuating'];
const REASONS = [
  '', 'Battery must read 3.4–4.35 V before a scenario can arm.', 'A vibration/pump sequence is already running.',
  'Release the SOS button first.', 'This scenario is not on the wearable.', 'The wearable’s scenario list changed. Reconnect.',
  'A scenario is already running.', 'No scenario is running.', 'Malformed command.',
  'Scenarios are disabled on this wearable (or its outputs are unavailable).', 'Release the Cancel button first.',
  'Cancelled on the wearable.', 'Aborted from the app.', 'Outputs are running: use Cancel or a double tap on the wearable.',
  'The output sequence refused the trigger (busy, cancelled or battery).',
];

export interface ScenarioEntry {
  id: number; version: number; name: string;
  irSamples: number; imuSamples: number; spo2Samples: number; contentCrc32: number;
  durationS: number;
}
export interface ScenarioCatalog { version: number; catalogCrc: number; entries: ScenarioEntry[] }
export interface ScenarioStatus {
  state: ScenarioState; reason: number; reasonText: string;
  commandToken: number; scenarioId: number; remainingMs: number; catalogCrc: number; runToken: number;
  pulseScore: number | null; oxygenConcern: number | null;
  pulseAlert: boolean; oxygenAlert: boolean; triggerAccepted: boolean; imuGateOpen: boolean;
  playbackFinished: boolean; scenariosCompiled: boolean;
}

export function decodeCatalog(view: DataView): ScenarioCatalog {
  if (view.byteLength < HEADER_BYTES) throw new Error('Scenario catalog too short.');
  const version = view.getUint8(0), count = view.getUint8(1), catalogCrc = view.getUint16(2, true);
  if (version !== SCENARIO_VERSION) throw new Error(`Unsupported scenario protocol version ${version}.`);
  if (view.byteLength !== HEADER_BYTES + count * ENTRY_BYTES) throw new Error('Scenario catalog length mismatch.');
  const entries: ScenarioEntry[] = [];
  for (let i = 0; i < count; i += 1) {
    const o = HEADER_BYTES + i * ENTRY_BYTES;
    const nameBytes = new Uint8Array(view.buffer, view.byteOffset + o + 16, NAME_BYTES);
    const end = nameBytes.indexOf(0);
    const name = new TextDecoder().decode(nameBytes.subarray(0, end < 0 ? NAME_BYTES : end));
    const irSamples = view.getUint32(o + 4, true), spo2Samples = view.getUint32(o + 8, true);
    entries.push({ id: view.getUint8(o), version: view.getUint8(o + 1), imuSamples: view.getUint16(o + 2, true),
      irSamples, spo2Samples, contentCrc32: view.getUint32(o + 12, true), name,
      durationS: Math.max(irSamples / 100, spo2Samples) });
  }
  return { version, catalogCrc, entries };
}

export function encodeCommand(opcode: 'start' | 'abort', token: number, scenarioId = 0, catalogCrc = 0): Uint8Array {
  if (!Number.isInteger(token) || token < 1 || token > 0xffff) throw new Error('Scenario command token must be 1..65535.');
  const bytes = new Uint8Array(COMMAND_BYTES);
  const view = new DataView(bytes.buffer);
  view.setUint8(0, opcode === 'start' ? 1 : 2);
  view.setUint8(1, SCENARIO_VERSION);
  view.setUint16(2, token, true);
  view.setUint8(4, opcode === 'start' ? scenarioId : 0);
  view.setUint8(5, 0);
  view.setUint16(6, opcode === 'start' ? catalogCrc : 0, true);
  return bytes;
}

const unit = (raw: number) => raw === 0xffff ? null : raw / 10000;

export function decodeStatus(view: DataView): ScenarioStatus {
  if (view.byteLength !== STATUS_BYTES || view.getUint8(0) !== 1) throw new Error('Invalid scenario status packet.');
  if (view.getUint8(1) !== SCENARIO_VERSION) throw new Error('Unsupported scenario status version.');
  const state = STATES[view.getUint8(2)];
  if (!state) throw new Error('Unknown scenario state.');
  const reason = view.getUint8(3), flags = view.getUint8(7);
  return {
    state, reason, reasonText: REASONS[reason] ?? `Reason ${reason}`,
    commandToken: view.getUint16(4, true), scenarioId: view.getUint8(6), remainingMs: view.getUint32(8, true),
    catalogCrc: view.getUint16(12, true), pulseScore: unit(view.getUint16(14, true)),
    oxygenConcern: unit(view.getUint16(16, true)), runToken: view.getUint16(18, true),
    pulseAlert: (flags & 1) !== 0, oxygenAlert: (flags & 2) !== 0, triggerAccepted: (flags & 4) !== 0,
    scenariosCompiled: (flags & 32) !== 0, playbackFinished: (flags & 64) !== 0, imuGateOpen: (flags & 128) !== 0,
  };
}

export const scenarioStateLabels: Record<ScenarioState, string> = {
  idle: 'Idle', armed: 'Armed · get in the water', playing: 'Playing scripted data', alerted: 'Alert raised',
  actuating: 'Vibration → pump running', done: 'Finished', aborted: 'Stopped', rejected: 'Not started',
};
