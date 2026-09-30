#pragma once
// Host-only clock shim for Frame3ds correctness tests. No GPU emulation or
// hardware timing is provided; these tests cannot establish console FPS.
#include <chrono>
#include <cstdint>
inline constexpr std::uint64_t SYSCLOCK_ARM11 = 1'000'000'000ULL;
inline std::uint64_t svcGetSystemTick() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline std::uint64_t osGetTime() { return svcGetSystemTick() / 1'000'000U; }
