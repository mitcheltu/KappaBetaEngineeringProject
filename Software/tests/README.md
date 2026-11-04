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

The sequence tests cover the ten-second vibration countdown, valve hold until
Cancel, cancellation before/during/exactly at opening, simultaneous buttons,
optional timed opening, and `millis()` wraparound. They also check that a held
SOS at boot cannot trigger the sequence, active presses cannot restart or extend
it, and stop/cancel, disabled controls, or an invalid/low battery take priority.
Recovery from an interlock requires a fresh release and press of SOS.

The BMI270 tests exercise the actual checked transport adapter with injected
I2C failures and configuration readback faults. Bosch API calls are test doubles;
the ESP32 compile uses Bosch's actual driver from the SparkFun library. These
tests do not establish physical sensor behavior or wiring.

Pressure conversion, PROM CRC, transport failure, and stale-value tests have
separate [instructions beside the checked MS5837 library](../libraries/KappaBeta_MS5837/README.md).
Create `tmp/revf-review` before running its example commands if it does not exist.
