# KappaBeta MS5837 checked transport

This local fork is based on [BlueRobotics MS5837 Library 1.1.1](https://github.com/bluerobotics/BlueRobotics_MS5837_Library). The original MIT license is retained. Install this folder as an Arduino library, or pass `--libraries Software/libraries` to Arduino CLI.

The class/header are named `KappaBetaMS5837`. Initialize the shared bus first, then call `init(Wire)` and explicitly select `MS5837_30BA`. `read()` returns `bool`; every I2C write, acknowledgement and requested byte count is checked. A failed read, failed initialization, or not-yet-completed first conversion returns `NaN` from all measurement getters, rather than exposing a previous measurement. Initialization also rejects all-zero/all-one coefficient sets and checks the original PROM CRC4. ADC zero/all-one results are rejected. This detects transport failures, not every possible physically incorrect reading.

The upstream conversion and CRC equations are retained. Arithmetic intermediates are widened for squared temperature differences to avoid 32-bit overflow (including the upstream high-temperature `dT*dT` expression). Default model and internal state are initialized. Changing the model invalidates the previous measurement. `init()` never calls `Wire.begin()` and never changes the selected bus pins or clock. Reads still block for two 20 ms conversion waits; the caller must serialize access to the shared bus and service the PPG FIFO frequently.

Run the host regression tests from the repository root with a C++17 compiler:

```powershell
g++ -std=c++17 -Wall -Wextra -I Software/libraries/KappaBeta_MS5837/tests/stubs -I Software/libraries/KappaBeta_MS5837/src Software/libraries/KappaBeta_MS5837/src/KappaBetaMS5837.cpp Software/libraries/KappaBeta_MS5837/tests/transport_test.cpp -o tmp/revf-review/pressure-tests.exe
./tmp/revf-review/pressure-tests.exe
```

The tests use the MS5837-30BA manufacturer's published C1-C6/D1/D2 example from the pressure calculation section, plus injected NACKs, failed writes, truncated reads, missing bytes, CRC mismatch, invalid ADC words, recovery and stale-value invalidation. They do not establish physical wiring or depth accuracy.
