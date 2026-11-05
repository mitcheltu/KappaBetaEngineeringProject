# Sensor history protocol v1

The MCU continuously records into a PSRAM ring. Overwriting removes one oldest
record per append. Sensor acquisition and ring/transfer state have one owner:
the Arduino loop. BLE callbacks only queue commands and update connection state.

## Records and capacity

Records are 32 bytes, little-endian. A 4 MiB ring holds 131,072 records. At the
upper scheduling budget of 50 IMU + 25 PPG + 5 depth + 5 system records/s, this
is 1,542 seconds (25.7 minutes). Actual IMU polling is reduced by blocking depth
reads. Events such as surface zero add a small number of records.

| Offset | Type | Meaning |
|---:|---|---|
| 0 | u32 | Record sequence, starting at zero for each boot session |
| 4 | u8 | Kind: 1 depth, 2 IMU, 3 PPG, 4 system, 5 surface-zero event |
| 5 | u8 | Existing live-packet validity/status flags; PPG bit 7 means estimated FIFO sample time |
| 6 | u16 | Reserved, zero |
| 8 | u64 | Monotonic acquisition time in microseconds, not calendar time |
| 16 | 16 bytes | Payload; unused bytes zero |

Kinds 1–4 use the same first 12 payload bytes as their existing live packet
(live offsets 8–19). Depth payload offset 12 additionally stores the surface
baseline in Pa as f32 (NaN if unset). Kind 5 contains baseline Pa as f32 and bit
0 indicates success. Raw PPG is logged per FIFO sample, including unusable optical
signals; estimates keep their own validity flags. Sampling failures are logged
with validity clear; absence/retry intervals are visible in timestamps/status.
PPG FIFO timestamps are backdated, so timestamps across different record kinds
need not increase with append sequence. The sequence is the transfer cursor.
Sequence IDs never wrap within a session: UINT32_MAX is the terminal exclusive
cursor and is never assigned to a record. Logging then stops with an explicit
exhaustion flag.

Two 1 MiB aligned PSRAM workspaces are reserved for future ML. They contain no
model/inference implementation. The runtime reports allocation success and
actual ring capacity. The ring falls back from 4 to 2 to 1 MiB if needed while
preserving at least 1 MiB of free PSRAM at allocation time. Both ML workspaces
must allocate before the ring is enabled. Large allocations never fall back to internal SRAM.
PSRAM history and these workspaces are lost on reset or power loss.

## GATT characteristics

All UUIDs share suffix `-7e57-4d8e-b6f4-889c1d4e8270` under the existing service.

| First group | Role | Properties |
|---|---|---|
| e81b6230 | Identity | READ, 20 bytes |
| e81b6231 | Transfer status | READ/NOTIFY, 20 bytes |
| e81b6232 | Data fragments | NOTIFY, at most negotiated ATT MTU minus 3 |
| e81b6233 | Commands | WRITE with response, exactly 8 bytes |

Identity: byte 0 version=1, byte 1 record size=32, byte 2 flags, byte 3 zero,
bytes 4–9 device MAC bytes, bytes 10–17 random boot-session nonce, bytes 18–19
zero. Session storage keys combine device ID and boot nonce. Flags: bit 0 history
allocated, bit 1 both ML workspaces reserved, bit 2 sequence exhausted.

Commands: byte 0 opcode, byte 1 version=1, u16 token at 2, u32 cursor at 4.
The browser chooses a nonzero token per transfer and writes commands serially.
Cursor always means the next record wanted/durably saved (exclusive ACK).

| Opcode | Meaning |
|---:|---|
| 1 OPEN | Start initial history transfer; gate live notifications; snapshot current next sequence |
| 2 ACK | Confirm current block/gap durably stored; cursor must equal its exact end |
| 3 LIVE | Acknowledge END at its exact snapshot boundary; enable live notifications |
| 4 POLL | Start another finite snapshot while retaining live mode, to persist samples recorded during transfer/live use |
| 5 ABORT | Cancel transfer; initial live gate remains closed unless LIVE was previously accepted |

Status: byte 0 type, byte 1 version=1, u16 token at 2, u32 first at 4,
u32 end at 8, u32 detail at 12, u16 count at 16, u16 flags at 18.
Status flags use the same allocation/ML/exhaustion bits as identity. Token zero
is idle status and must be ignored by clients awaiting their transfer token.

| Type | first / end / detail / count |
|---:|---|
| 1 READY | Current oldest / fixed snapshot end / ring capacity / 0 |
| 2 BLOCK | Block first / fixed snapshot end / CRC32 of raw record bytes / record count (1–64) |
| 3 END | Snapshot end / snapshot end / 0 / 0 |
| 4 GAP | Missing first / next available sequence (capped at snapshot end) / 0 / 0 |
| 5 LIVE | Accepted cursor / current next sequence / capacity / 0 |
| 6 ERROR | Error code / snapshot end / 0 / 0 |

Error codes: 1 bad cursor, 2 transfer timeout, 3 invalid command/state.
Unknown/wrong-version commands are ignored. Wrong-token, stale-connection and
out-of-order ACKs never advance state. OPEN/POLL with a cursor beyond current
next returns ERROR. No history allocation yields READY(capacity=0) then END,
allowing explicitly labelled live-only operation.

Data fragment: u16 token at 0, u16 byte offset within block at 2, u32 block-first
sequence at 4, then data bytes. The MCU stages up to 64 records (2,048 bytes)
immutably until ACK, so ring overwrite cannot alter a retry. Frames work at the
default MTU 23 (12 data bytes/frame) and larger negotiated MTUs (requested 247).
CRC32 is reflected IEEE polynomial 0xedb88320, init/final XOR 0xffffffff.

## Transfer and persistence

The client reads identity, subscribes to status/data, and loads its IndexedDB
checkpoint before OPEN. READY gives a fixed transfer target. BLOCK precedes its
fragments. READY is informational: a lost READY must not prevent processing
BLOCK, GAP or END for the active token. The client verifies length, all fragment ranges, CRC and consecutive
record IDs. It atomically commits the records and next cursor in IndexedDB,
then ACKs. GAP is likewise committed with its new cursor before ACK. Identical
retries are idempotent; changed/corrupt data is never acknowledged.

The MCU retries status/data after 2 seconds without progress or ACK, up to 5
retries, then reports ERROR. Each successfully enqueued fragment refreshes the
progress timer; the ACK timer starts after the final fragment. Failed notify enqueue is retried without advancing fragment
offset. Sampling and local actuator control continue during transfer/retries.
The client also times out stalled links; storage/quota failures stop transfer
without ACK or silently enabling live mode.

After initial END, the client writes LIVE, waits for LIVE status, then enables
the existing live telemetry subscriptions. It continues POLL transfers about
every 250 ms from the durable cursor, including the tail recorded during the
initial download. This preserves full-rate recorded samples alongside the 5 Hz
display stream. Later END/LIVE exchanges keep live subscriptions active.
The UI distinguishes initial download from live plus background catch-up.
LIVE is idempotent: a repeated command returns the cached acceptance. Its status
may retry up to five times and then becomes quiet, because it requires no ACK.
POLL can begin while that previous LIVE acceptance is still being retried.

Only one connection owns transfer state. Disconnect cancels only the transfer,
not recording or actuator control. Reconnect opens from the browser's durable
checkpoint. Overwritten ranges are explicit gaps, never silently treated as
downloaded records. A slow link can fall behind the ring; no throughput or
lossless-retention guarantee is implied by successful notification enqueue.
Stored samples remain on the MCU until normal ring overwrite; ACK does not erase
them. Browser records are local, indexed by session and record ID, and exportable.
