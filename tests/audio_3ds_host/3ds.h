#pragma once
// Unit-test fake for Audio3ds PCM-ring writes only. This is not a console
// emulator: it supplies no SPC, GPU, scheduling or hardware performance model.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Result = std::int32_t;
using Thread = void*;
struct LightEvent {};
inline constexpr auto U64_MAX = UINT64_MAX;
inline constexpr int RESET_ONESHOT = 0;
inline constexpr int SOUND_REPEAT = 1, SOUND_FORMAT_16BIT = 2, SOUND_LINEAR_INTERP = 4;
inline constexpr u32 csndChannels = (1U << 8) | (1U << 9);
inline constexpr u32 CSND_TIMER(u32 rate) { return 0x3FEC3FCU / rate; }
inline constexpr bool R_SUCCEEDED(Result result) { return result >= 0; }
inline constexpr bool R_FAILED(Result result) { return result < 0; }
namespace audio_test {
inline u64 now_ms{};
inline std::int16_t* left{};
inline std::int16_t* right{};
inline std::size_t bytes{};
inline std::vector<std::uint64_t> flushes;
inline int flush_fail_after{-1};
inline Result play_result{};
}
inline u64 osGetTime() { return audio_test::now_ms; }
inline Result csndInit() { return 0; }
inline void csndExit() {}
inline void* linearAlloc(std::size_t bytes) { return std::malloc(bytes); }
inline void linearFree(void* pointer) { std::free(pointer); }
inline void LightEvent_Init(LightEvent*, int) {}
inline void LightEvent_Wait(LightEvent*) {}
inline void LightEvent_Signal(LightEvent*) {}
inline Result APT_CheckNew3DS(bool* result) { *result = false; return 0; }
inline Thread threadCreate(void (*)(void*), void*, std::size_t, int, int, bool) { return nullptr; }
inline Result threadJoin(Thread, u64) { return 0; }
inline void threadFree(Thread) {}
inline Result APT_GetAppCpuTimeLimit(u32* result) { *result = 0; return 0; }
inline Result APT_SetAppCpuTimeLimit(u32) { return 0; }
inline void CSND_SetPlayState(int, int) {}
inline Result CSND_UpdateInfo(bool) { return 0; }
inline Result GSPGPU_FlushDataCache(void* pointer, u32 bytes) {
    const auto base = reinterpret_cast<std::uintptr_t>(pointer);
    const auto left = reinterpret_cast<std::uintptr_t>(audio_test::left);
    const bool is_left = base >= left && base < left + audio_test::bytes;
    if (audio_test::left != nullptr) {
        const auto start = is_left ? left : reinterpret_cast<std::uintptr_t>(audio_test::right);
        audio_test::flushes.push_back((std::uint64_t{is_left} << 63)
            | ((base - start) << 32) | bytes);
    }
    if (audio_test::flush_fail_after >= 0
        && audio_test::flush_fail_after-- == 0) return -1;
    return 0;
}
inline Result csndPlaySound(int channel, int, int, float, float,
    void* data, void*, u32 bytes) {
    if (channel == 8) audio_test::left = static_cast<std::int16_t*>(data);
    else audio_test::right = static_cast<std::int16_t*>(data);
    audio_test::bytes = bytes;
    return audio_test::play_result;
}
