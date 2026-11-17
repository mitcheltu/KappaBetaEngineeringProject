# Stock MAX3010x FIFO contract test

This test compiles the **unchanged SparkFun MAX3010x Pulse and Proximity Sensor
Library 1.1.2** source against small Arduino/I2C stubs. It verifies the behavior
that `Software/testsAndDemo/DrowningWearableDemo/Sensors.ino` relies on:

- A newly received pair is consumed by calling `nextSample()` before the FIFO
  getters. Red and IR remain paired, including when the queue wraps.
- The four-slot software ring represents at most three unread pairs.
- A `check()` result of four aliases an empty software queue; five arrivals can
  appear as one unread pair. The demo must reject batches of four or more.
- Draining the software queue and calling the stock `clearFIFO()` permits fresh
  samples to be consumed after an overflow.
- The demo's actual configuration readback rejects a lost setup write, shutdown,
  incomplete reset, incorrect sampling/averaging/rollover/LED settings, and
  failed register reads even when an earlier chip-ID check succeeded.
- Configuration diagnostics do not drain optical FIFO data.
- The observed `WR=RD=0, OVF=25` state exposes the stock reader's full/empty
  ambiguity. The actual polling helper detects lost samples before `check()`
  can treat it as empty, clears the queues and accepts a fresh pair afterward.
- Overflow is checked before a FIFO pop can clear its counter; overdue service
  and batches that wrap the software ring remain discontinuities.

The stub supplies deterministic register values and FIFO data. This verifies
the installed library's queue implementation, not physical sensor timing, I2C
electrical behavior, HR/SpO2 accuracy, or the demo's actual achieved sample rate.
Keep this test when upgrading the library because its FIFO behavior may change.

Run from the repository root with a C++17 compiler and the pinned library:

```powershell
$maxLibrarySrc = Join-Path $env:USERPROFILE 'OneDrive/Documents/Arduino/libraries/SparkFun_MAX3010x_Pulse_and_Proximity_Sensor_Library/src'
New-Item -ItemType Directory -Force -Path './tmp/demo-revi' | Out-Null
g++ -std=c++17 -Wall -Wextra -DARDUINO=10800 -I Software/tests/demo_stock_ppg -I Software/testsAndDemo/DrowningWearableDemo -I $maxLibrarySrc Software/tests/demo_stock_ppg/test.cpp "$maxLibrarySrc/MAX30105.cpp" -o tmp/demo-revi/demo-stock-ppg.exe
if ($LASTEXITCODE -ne 0) { throw 'Stock MAX3010x FIFO test compilation failed' }
./tmp/demo-revi/demo-stock-ppg.exe
if ($LASTEXITCODE -ne 0) { throw 'Stock MAX3010x FIFO test failed' }
```

Set `$maxLibrarySrc` to your installed library's `src` directory if it is in a
different location. On Windows/MSYS2, put the compiler's `bin` directory on
`PATH`, for example `$env:PATH = 'C:/msys64/ucrt64/bin;' + $env:PATH`.
Do not compile with `-DNDEBUG`; this test uses assertions. The original library
currently emits an unrelated unused-variable warning in `enableSlot()`.
