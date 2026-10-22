#pragma once
// Unit-test fake for Audio3ds PCM-ring writes only. This is not a console
// emulator: it supplies no SPC, GPU, scheduling or hardware performance model.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <atomic>
#include <thread>
#include <utility>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Result = std::int32_t;
using Thread = void*;
struct LightEvent {};
inline constexpr auto U64_MAX = UINT64_MAX;
inline constexpr u32 CUR_PROCESS_HANDLE = 0xffff8001;
inline constexpr int RESET_ONESHOT = 0;
inline constexpr int SOUND_REPEAT = 1, SOUND_FORMAT_16BIT = 2, SOUND_LINEAR_INTERP = 4;
inline constexpr u32 csndChannels = (1U << 8) | (1U << 9);
inline constexpr u32 CSND_TIMER(u32 rate) { return 0x3FEC3FCU / rate; }
inline constexpr bool R_SUCCEEDED(Result result) { return result >= 0; }
inline constexpr bool R_FAILED(Result result) { return result < 0; }
enum APT_HookType { APTHOOK_ONSUSPEND,APTHOOK_ONRESTORE,APTHOOK_ONSLEEP,APTHOOK_ONWAKEUP };
using aptHookFn=void (*)(APT_HookType,void*);
struct aptHookCookie {};
namespace audio_test {
inline aptHookFn apt_callback{};
inline void* apt_param{};
inline u64 now_ms{};
inline bool new_3ds{};
inline Result model_result{};
inline unsigned quota_calls{};
inline std::vector<int> requested_cores;
// Opt-in APT slice and thread models; by default no slice is granted and no
// worker thread starts.
inline bool honour_quota{};
inline bool thread_succeeds{};
inline u32 cpu_quota{};
inline std::vector<u32> quota_requests;
inline std::int16_t* left{};
inline std::int16_t* right{};
inline std::size_t bytes{};
inline std::vector<std::uint64_t> flushes;
inline int flush_fail_after{-1};
inline Result play_result{};
inline std::atomic_bool csnd_completion_available{true};
inline std::atomic_uint csnd_completion_waits{};
inline std::vector<std::pair<int,int>> play_states;
}
inline void aptHook(aptHookCookie*,aptHookFn callback,void* param) {
    audio_test::apt_callback=callback;audio_test::apt_param=param;
}
inline void aptUnhook(aptHookCookie*) { audio_test::apt_callback=nullptr;audio_test::apt_param=nullptr; }
inline u64 osGetTime() { return audio_test::now_ms; }
namespace audio_test {
inline bool home_pending{};
inline unsigned main_loop_calls{};
inline std::size_t play_states_at_main_loop{};
}
inline bool aptShouldJumpToHome() { return audio_test::home_pending; }
inline bool aptMainLoop() {
    ++audio_test::main_loop_calls;
    audio_test::play_states_at_main_loop = audio_test::play_states.size();
    return true;
}
inline Result csndInit() { return 0; }
inline void csndExit() {}
inline void* linearAlloc(std::size_t bytes) { return std::malloc(bytes); }
inline void linearFree(void* pointer) { std::free(pointer); }
inline void LightEvent_Init(LightEvent*, int) {}
inline void LightEvent_Wait(LightEvent*) {}
inline void LightEvent_Signal(LightEvent*) {}
inline Result APT_CheckNew3DS(bool* result) { *result = audio_test::new_3ds; return audio_test::model_result; }
inline Thread threadCreate(void (*)(void*), void*, std::size_t, int, int core, bool) {
    audio_test::requested_cores.push_back(core);
    return audio_test::thread_succeeds ? static_cast<Thread>(&audio_test::requested_cores) : nullptr;
}
inline Result threadJoin(Thread, u64) { return 0; }
inline void threadFree(Thread) {}
inline Result APT_GetAppCpuTimeLimit(u32* result) {
    *result = audio_test::honour_quota ? audio_test::cpu_quota : 0U;
    return 0;
}
inline Result APT_SetAppCpuTimeLimit(u32 percent) {
    ++audio_test::quota_calls;
    audio_test::quota_requests.push_back(percent);
    if (audio_test::honour_quota) audio_test::cpu_quota = percent;
    return 0;
}
inline void CSND_SetPlayState(int channel, int state) { audio_test::play_states.emplace_back(channel,state); }
inline Result CSND_UpdateInfo(bool wait) {
    if(wait) {
        ++audio_test::csnd_completion_waits;
        while(!audio_test::csnd_completion_available) std::this_thread::yield();
    }
    return 0;
}
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
inline Result svcFlushProcessDataCache(u32 process, std::uintptr_t address, u32 bytes) {
    if(process!=CUR_PROCESS_HANDLE) return -1;
    return GSPGPU_FlushDataCache(reinterpret_cast<void*>(address),bytes);
}
