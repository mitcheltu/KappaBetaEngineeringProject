# Firmware host tests

Run these commands from the repository root with a C++17 compiler on `PATH`.
The examples use PowerShell and `g++`; change `$bmiLibraryDirectory` to the root
of your installed **SparkFun BMI270 Arduino Library 1.0.3**. Its `src` folder
must contain `bmi270_api/bmi270.h`.

```powershell
$bmiLibraryDirectory = './tmp/revf-build/libraries/SparkFun_BMI270_Arduino_Library-1.0.3'
New-Item -ItemType Directory -Force -Path './tmp/revf-tests' | Out-Null

g++ -std=c++17 -Wall -Wextra -Werror -I Software Software/tests/bench_sequence_test.cpp -o tmp/revf-tests/bench-sequence.exe
if ($LASTEXITCODE -ne 0) { throw 'Bench sequence test compilation failed' }
./tmp/revf-tests/bench-sequence.exe
if ($LASTEXITCODE -ne 0) { throw 'Bench sequence tests failed' }

g++ -std=c++17 -Wall -Wextra -Werror -I Software Software/tests/history_test.cpp -o tmp/revf-tests/history.exe
if ($LASTEXITCODE -ne 0) { throw 'History test compilation failed' }
./tmp/revf-tests/history.exe
if ($LASTEXITCODE -ne 0) { throw 'History tests failed' }

$bmiIncludeDirectory = Join-Path $bmiLibraryDirectory 'src'
g++ -std=c++17 -Wall -Wextra -I Software/tests/bmi/stubs -I Software -I $bmiIncludeDirectory Software/tests/checked_bmi270_test.cpp -o tmp/revf-tests/checked-bmi270.exe
if ($LASTEXITCODE -ne 0) { throw 'BMI270 test compilation failed' }
./tmp/revf-tests/checked-bmi270.exe
if ($LASTEXITCODE -ne 0) { throw 'BMI270 tests failed' }
```

For MSYS2 on Windows, the compiler's `bin` directory must be on `PATH` so its
runtime DLLs can load. For a standard UCRT64 installation:

```powershell
$env:PATH = 'C:/msys64/ucrt64/bin;' + $env:PATH
```

Do not compile these assertion-based tests with `-DNDEBUG`.

The history tests verify the wire layouts and IEEE CRC32, externally allocated
ring storage, chronological overwrite, capacity and sequence exhaustion,
finite snapshots while samples continue, immutable retries after overwrite,
default-MTU fragmentation, failed notification enqueue, stale/wrong/premature
ACK rejection, explicit gaps, live-mode handshakes, reconnect cancellation and
timeouts across `millis()` rollover. They exercise the pure single-owner helpers;
they do not establish PSRAM availability, BLE throughput or browser durability.

`history-protocol-fixture.json` is generated from the firmware's actual C++ wire
types and transfer helper, then consumed by browser parser tests. It includes
all five record kinds, signed IMU values, timestamps above JavaScript's exact
integer range, an IEEE CRC32, an ACK command and default-MTU fragments. Regenerate
it after an intentional protocol change (do not use `-DNDEBUG`):

```powershell
g++ -std=c++17 -Wall -Wextra -Werror -I Software Software/tests/history_fixture.cpp -o tmp/revf-tests/history-fixture.exe
if ($LASTEXITCODE -ne 0) { throw 'History fixture compilation failed' }
$fixtureJson = ./tmp/revf-tests/history-fixture.exe
if ($LASTEXITCODE -ne 0) { throw 'History fixture generation failed' }
$fixtureJson | Set-Content -LiteralPath Software/tests/history-protocol-fixture.json -Encoding utf8
```

The sequence tests cover the ten-second vibration countdown, pump run until
Cancel, cancellation before/during/exactly at pump start, simultaneous buttons,
optional timed running, and `millis()` wraparound. They also check that a held
SOS at boot cannot trigger the sequence, active presses cannot restart or extend
it, and stop/cancel, disabled controls, or an invalid/low battery take priority.
Recovery from an interlock requires a fresh release and press of SOS.

The BMI270 tests exercise the actual checked transport adapter with injected
I2C failures and configuration readback faults. Bosch API calls are test doubles;
the ESP32 compile uses Bosch's actual driver from the SparkFun library. These
tests do not establish physical sensor behavior or wiring.

Depth readings now use the unmodified **BlueRobotics MS5837 Library 1.1.1**.
The former custom-driver tests were removed with that driver; they do not
describe the official library's error handling. The ESP32 build verifies API
compatibility. The sketch checks device acknowledgement before and after reads
and rejects nonfinite/out-of-range results, but the upstream `void read()` does
not expose individual transfer failures. Bench-check depth/temperature against
a reference and check disconnect/reconnect behavior; software checks alone
cannot establish measurement accuracy. Installation and build settings are in
the [electronics documentation](../../Electrical/README.md).
