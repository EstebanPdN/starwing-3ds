#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
typedef int LightLock;
#define BIT(n) (1U<<(n))
enum { GFX_TOP, GFX_LEFT=0, GSP_RGB565_OES=2 };
typedef struct gxCmdQueue {
    unsigned numEntries, curEntry, lastEntry;
    bool stopped;
    void (*callback)(struct gxCmdQueue*);
} gxCmdQueue_s;
#define SYSCLOCK_ARM11 268123480ULL
#ifdef __cplusplus
extern "C" {
#endif
u64 svcGetSystemTick(void);
void svcSleepThread(s64);
bool gxCmdQueueWait(gxCmdQueue_s*,s64);
void gxCmdQueueStop(gxCmdQueue_s*);
void gxCmdQueueClear(gxCmdQueue_s*);
void LightLock_Lock(LightLock*);
void LightLock_Unlock(LightLock*);
void* linearAlloc(size_t);
u8* gfxGetFramebuffer(int,int,u16*,u16*);
void gfxScreenSwapBuffers(int,bool);
bool gspPresentBuffer(unsigned,unsigned,const void*,const void*,u32,u32);
#ifdef __cplusplus
}
#endif
