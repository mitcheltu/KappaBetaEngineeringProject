#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
using byte = uint8_t;
using boolean = bool;
extern uint32_t testMillis;
inline uint32_t millis() { return testMillis; }
inline void delay(uint32_t ms) { testMillis += ms; }
