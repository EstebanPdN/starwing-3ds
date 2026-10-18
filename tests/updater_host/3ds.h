#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Result = int;
using Handle = u32;
using LightLock = std::mutex;
using Thread = void*;
enum FS_MediaType { MEDIATYPE_SD };
struct FS_Path { const void* data; };
struct FS_ArchiveResource { u32 freeClusters = 1U << 20, clusterSize = 4096; };
struct AM_TitleInfo { u64 titleID; u32 size; std::uint16_t version; };
inline bool R_SUCCEEDED(Result r) { return r >= 0; }
inline bool R_FAILED(Result r) { return r < 0; }
inline void LightLock_Init(LightLock*) {}
inline void LightLock_Lock(LightLock* l) { l->lock(); }
inline void LightLock_Unlock(LightLock* l) { l->unlock(); }
inline bool envIsHomebrew() { return false; }
inline bool aptShouldClose() { return false; }
inline bool aptIsActive() { return true; }
inline bool aptIsHomeAllowed() { return true; }
inline bool aptIsSleepAllowed() { return true; }
inline void aptSetHomeAllowed(bool) {}
inline void aptSetSleepAllowed(bool) {}
inline Thread threadCreate(void(*)(void*), void*, std::size_t, int, int, bool) { return nullptr; }
inline void threadJoin(Thread, u64) {}
inline void threadFree(Thread) {}
inline Result acInit() { return 0; }
inline void acExit() {}
inline Result ACU_GetWifiStatus(u32* out) { *out = 1; return 0; }
inline Result socInit(u32*, u32) { return 0; }
inline void socExit() {}
inline Result sslcInit(int) { return 0; }
inline void sslcExit() {}
inline constexpr int ARCHIVE_SDMC = 0, PATH_EMPTY = 0, PATH_ASCII = 1, FS_OPEN_READ = 1;
inline FS_Path fsMakePath(int, const void* p) { return {p}; }
Result FSUSER_GetSdmcArchiveResource(FS_ArchiveResource*);
Result FSUSER_OpenFileDirectly(Handle*, int, FS_Path, FS_Path, u32, u32);
Result FSFILE_Read(Handle, u32*, u64, void*, u32);
Result FSFILE_Write(Handle, u32*, u64, const void*, u32, u32);
Result FSFILE_Close(Handle);
Result amInit();
void amExit();
Result AM_GetCiaFileInfo(FS_MediaType, AM_TitleInfo*, Handle);
Result AM_GetCiaRequiredSpace(u64*, FS_MediaType, Handle);
Result AM_StartCiaInstallOverwrite(Handle*, FS_MediaType);
Result AM_CancelCIAInstall(Handle);
Result AM_FinishCiaInstall(Handle);
