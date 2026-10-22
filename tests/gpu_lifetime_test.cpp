#include <3ds.h>
#include "../platform/3ds/source/gpu_lifetime_3ds.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
extern "C" void __wrap_GX_BindQueue(gxCmdQueue_s*);
extern "C" void __wrap_C3Di_RenderQueueWaitDone(void);
extern "C" void __wrap_gxCmdQueueRun(gxCmdQueue_s*);
extern "C" bool __wrap_gxCmdQueueWait(gxCmdQueue_s*,s64);
static u64 ticks;
static int clears, stops, sleeps, runs, swaps;
static bool finish_after_sleep;
static gxCmdQueue_s* bound;
static std::mutex callback_mutex;
static bool callback_locked, fail_allocation=true;
static std::atomic_bool block_swap, entered_swap, release_swap;
static unsigned allocations, cpu_presents;
static const void* scanned;
static unsigned scan_id;
static u8 old_scanout[400*240*2];
extern "C" void LightLock_Lock(LightLock*) { callback_mutex.lock(); assert(!callback_locked); callback_locked=true; }
extern "C" void LightLock_Unlock(LightLock*) { assert(callback_locked); callback_locked=false; callback_mutex.unlock(); }
extern "C" void* linearAlloc(size_t bytes) { ++allocations; return fail_allocation?nullptr:std::malloc(bytes); }
extern "C" u8* gfxGetFramebuffer(int,int,u16* w,u16* h) {
    if(w)*w=240;if(h)*h=400;return old_scanout;
}
extern "C" void gfxScreenSwapBuffers(int,bool) { ++cpu_presents; }
extern "C" bool gspPresentBuffer(unsigned screen,unsigned id,const void* a,const void* b,u32 stride,u32 mode) {
    assert(screen==GFX_TOP && a==b && stride==480 && mode==(GSP_RGB565_OES|BIT(6)|BIT(8)));
    scanned=a;scan_id=id;++cpu_presents;return false;
}
static void swap(gxCmdQueue_s*) {
    assert(callback_locked);
    if (block_swap) {
        entered_swap=true;
        while (!release_swap) std::this_thread::yield();
    }
    ++swaps;
}
extern "C" void __real_GX_BindQueue(gxCmdQueue_s* q) { bound=q; }
extern "C" void __real_gxCmdQueueRun(gxCmdQueue_s* q) { ++runs; q->curEntry=q->numEntries; }
extern "C" u64 svcGetSystemTick() { return ticks; }
extern "C" void svcSleepThread(s64) {
    ++sleeps; ticks+=SYSCLOCK_ARM11/2000;
    if (finish_after_sleep) {
        bound->lastEntry=bound->numEntries;
        if (bound->callback) bound->callback(bound);
    }
}
extern "C" bool __real_gxCmdQueueWait(gxCmdQueue_s* q,s64 timeout) {
    assert(timeout==0); return q->lastEntry==q->numEntries;
}
extern "C" void gxCmdQueueStop(gxCmdQueue_s* q) { ++stops; q->stopped=true; }
extern "C" void gxCmdQueueClear(gxCmdQueue_s* q) {
    assert(q->lastEntry==q->numEntries); ++clears;
    q->numEntries=q->curEntry=q->lastEntry=0;
}
int main(int argc,char**) {
    if (argc>1) {
        gxCmdQueue_s q{};q.callback=swap;q.numEntries=3;
        __wrap_GX_BindQueue(&q);__wrap_gxCmdQueueRun(&q);
        q.lastEntry=3;block_swap=true;
        std::thread completion([&]{q.callback(&q);});
        while(!entered_swap) std::this_thread::yield();
        std::atomic_bool attempted{},returned{};
        std::thread recovery([&]{attempted=true;starwing_abandon_gpu();returned=true;});
        while(!attempted) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        assert(!returned && !starwing_gpu_abandoned());
        release_swap=true;completion.join();recovery.join();
        assert(returned && starwing_gpu_abandoned() && swaps==1);
        q.callback(&q);assert(swaps==1);
        puts("PASS: concurrent abandonment waits for an entered swap, later swaps suppressed");
        return 0;
    }
    assert(!starwing_prepare_cpu_top());
    fail_allocation=false;
    assert(starwing_prepare_cpu_top() && starwing_prepare_cpu_top() && allocations==2);
    assert(starwing_cpu_top_framebuffer(nullptr,nullptr)==old_scanout);
    starwing_swap_cpu_top();assert(cpu_presents==1);
    gxCmdQueue_s q{}; q.callback=swap; __wrap_GX_BindQueue(&q);
    __wrap_gxCmdQueueRun(&q); assert(runs==0); // no unguarded auto-start via Add
    __wrap_C3Di_RenderQueueWaitDone();
    assert(clears==1 && !starwing_gpu_abandoned());
    q.numEntries=3; __wrap_gxCmdQueueRun(&q); assert(runs==1);
    q.lastEntry=3; // GPU idle, but the GSP thread has not performed its swap.
    assert(!__wrap_gxCmdQueueWait(&q,0) && swaps==0);
    q.callback(&q);
    assert(__wrap_gxCmdQueueWait(&q,0) && swaps==1);
    __wrap_C3Di_RenderQueueWaitDone(); assert(clears==2);
    q.numEntries=3; __wrap_gxCmdQueueRun(&q); finish_after_sleep=true;
    __wrap_C3Di_RenderQueueWaitDone();
    assert(clears==3 && sleeps==1 && swaps==2 && !starwing_gpu_abandoned());
    q.numEntries=20; q.curEntry=3; q.lastEntry=0;
    __wrap_gxCmdQueueRun(&q); q.curEntry=3; // three in flight, seventeen pending
    finish_after_sleep=false; ticks=0; sleeps=0;
    assert(!__wrap_gxCmdQueueWait(&q,1000000));
    assert(ticks>=1000000 && ticks<1000000+SYSCLOCK_ARM11/2000 && !starwing_gpu_abandoned());
    ticks=0;
    __wrap_C3Di_RenderQueueWaitDone();
    assert(starwing_gpu_abandoned() && q.stopped && clears==3);
    assert(q.curEntry==3 && q.numEntries==20 && q.lastEntry==0);
    assert(ticks>=SYSCLOCK_ARM11/4 && ticks<SYSCLOCK_ARM11/3);
    const auto before=ticks; const int old_stops=stops, old_runs=runs;
    __wrap_C3Di_RenderQueueWaitDone(); starwing_abandon_gpu();
    assert(ticks==before && stops==old_stops && clears==3);
    // Late completion cannot swap the stale PICA image over the CPU fallback.
    q.lastEntry=q.numEntries; q.callback(&q);
    __wrap_gxCmdQueueRun(&q); __wrap_C3Di_RenderQueueWaitDone();
    assert(q.numEntries==20 && clears==3 && swaps==2 && runs==old_runs);
    u16 w{},h{};
    auto* first=starwing_cpu_top_framebuffer(&w,&h);
    assert(first && first!=old_scanout && w==240 && h==400);
    std::memset(first,0x5a,sizeof(old_scanout));starwing_swap_cpu_top();
    assert(scanned==first && scan_id==0);
    auto* second=starwing_cpu_top_framebuffer(nullptr,nullptr);
    assert(second!=first && second!=old_scanout);
    std::memset(second,0xa5,sizeof(old_scanout));starwing_swap_cpu_top();
    assert(scanned==second && scan_id==1 && cpu_presents==3);
    // An already-submitted transfer may still write its original destination.
    std::memset(old_scanout,0xff,sizeof(old_scanout));
    for(unsigned i=0;i<sizeof(old_scanout);++i) assert(first[i]==0x5a && second[i]==0xa5);
    assert(starwing_cpu_top_framebuffer(nullptr,nullptr)==first && allocations==2);
    puts("PASS: GPU callback fence, tick timeouts, bounded suspend, late completion/write quarantine, separate CPU scanout");
}
