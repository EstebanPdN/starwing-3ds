#pragma once
// Allocation/cache/submission contract fake. It does not emulate PICA or
// establish device pixels, GPU stability or console performance.
#include <cstdint>
using u32=std::uint32_t;using u64=std::uint64_t;using Result=int;
constexpr u32 CUR_PROCESS_HANDLE=0xffff8001U;
constexpr u64 SYSCLOCK_ARM11=1'000'000;
inline u64 svcGetSystemTick(){static u64 tick=0;return ++tick;}
constexpr bool R_SUCCEEDED(Result r){return r>=0;}
constexpr bool R_FAILED(Result r){return r<0;}
namespace pica_test { inline bool cache_fail=false;inline unsigned cleans=0; }
inline Result svcStoreProcessDataCache(u32,u32,u32){++pica_test::cleans;return pica_test::cache_fail?-1:0;}
inline Result GSPGPU_FlushDataCache(const void*,u32){++pica_test::cleans;return pica_test::cache_fail?-1:0;}
