#pragma once
#include <cstdint>
// The acquisition test does not call this; analysis substitutes known estimates
// so task scheduling/latching can be checked independently from Maxim's math.
void maxim_heart_rate_and_oxygen_saturation(std::uint32_t*, std::int32_t,
    std::uint32_t*, std::int32_t*, std::int8_t*, std::int32_t*, std::int8_t*);
