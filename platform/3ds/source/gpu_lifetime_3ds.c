#include "gpu_lifetime_3ds.h"
#include <3ds.h>
#include <stdatomic.h>

// Linker seams around Citro3D 1.7.1's queue binding and APT suspend fence.
// Keep the public queue handle; no assumptions about C3D_Context layout.
static gxCmdQueue_s* render_queue;
static atomic_bool abandoned;
static atomic_bool callback_pending;
static void (*render_callback)(gxCmdQueue_s*);
static LightLock callback_lock = 1;
// Process-owned reserve: these buffers are never submitted as GX destinations.
// Keep them across game changes, including after a permanently lost context.
enum { CPU_TOP_BYTES = 400 * 240 * 2 };
static u8* cpu_top_buffers;
static unsigned cpu_top_back;
void __real_GX_BindQueue(gxCmdQueue_s* queue);
void __real_gxCmdQueueRun(gxCmdQueue_s* queue);
bool __real_gxCmdQueueWait(gxCmdQueue_s* queue, s64 timeout);

void __wrap_GX_BindQueue(gxCmdQueue_s* queue) {
    render_queue = queue;
    __real_GX_BindQueue(queue);
}

bool starwing_gpu_abandoned(void) { return atomic_load_explicit(&abandoned, memory_order_acquire); }

static void finish_render_queue(gxCmdQueue_s* queue) {
    // libctru exposes the queue as idle before invoking this callback. Its
    // screen swap must finish before the next frame reuses either framebuffer.
    LightLock_Lock(&callback_lock);
    if (!starwing_gpu_abandoned() && render_callback) render_callback(queue);
    LightLock_Unlock(&callback_lock);
    atomic_store_explicit(&callback_pending, false, memory_order_release);
}

void __wrap_gxCmdQueueRun(gxCmdQueue_s* queue) {
    if (queue == render_queue) {
        if (starwing_gpu_abandoned() || queue->lastEntry >= queue->numEntries) return;
        if (queue->callback != finish_render_queue) {
            render_callback = queue->callback;
            queue->callback = finish_render_queue;
        }
        atomic_store_explicit(&callback_pending, true, memory_order_release);
    }
    // An empty queue must stay inactive: otherwise Add can start commands
    // without going through the callback fence above.
    __real_gxCmdQueueRun(queue);
}

bool __wrap_gxCmdQueueWait(gxCmdQueue_s* queue, s64 timeout) {
    if (queue != render_queue) return __real_gxCmdQueueWait(queue, timeout);
    const u64 start = svcGetSystemTick();
    for (;;) {
        if (starwing_gpu_abandoned()) return false;
        if (__real_gxCmdQueueWait(queue, 0)
            && !atomic_load_explicit(&callback_pending, memory_order_acquire)) return true;
        if (timeout == 0) return false;
        const u64 elapsed = svcGetSystemTick() - start;
        // libctru's gxCmdQueueWait timeout is in system ticks, unlike SVC waits.
        if (timeout > 0 && elapsed >= (u64)timeout) return false;
        svcSleepThread(500000);
    }
}

void starwing_abandon_gpu(void) {
    // Serialize with an already-entered completion callback. On return no old
    // swap may still publish a GX-owned framebuffer over CPU recovery.
    LightLock_Lock(&callback_lock);
    const bool was_abandoned = atomic_exchange_explicit(&abandoned, true, memory_order_acq_rel);
    LightLock_Unlock(&callback_lock);
    if (was_abandoned) return;
    // Stop submission of pending commands. Already-submitted GPU commands
    // can still consume their buffers: all GPU allocations stay alive until
    // process teardown. A real late interrupt retires the stopped queue.
    if (render_queue) gxCmdQueueStop(render_queue);
}

bool starwing_prepare_cpu_top(void) {
    if (!cpu_top_buffers) cpu_top_buffers = linearAlloc(2U * CPU_TOP_BYTES);
    return cpu_top_buffers != NULL;
}

u8* starwing_cpu_top_framebuffer(u16* width, u16* height) {
    if (!starwing_gpu_abandoned()) return gfxGetFramebuffer(GFX_TOP, GFX_LEFT, width, height);
    if (width) *width = 240;
    if (height) *height = 400;
    return cpu_top_buffers ? cpu_top_buffers + cpu_top_back * CPU_TOP_BYTES : NULL;
}

void starwing_swap_cpu_top(void) {
    if (!starwing_gpu_abandoned()) {
        gfxScreenSwapBuffers(GFX_TOP, false);
        return;
    }
    if (!cpu_top_buffers) return;
    const u8* pixels = cpu_top_buffers + cpu_top_back * CPU_TOP_BYTES;
    // RGB565, 2D top screen, linear-memory scanout (same public GSP mode as gfx).
    gspPresentBuffer(GFX_TOP, cpu_top_back, pixels, pixels, 240U * 2U,
        GSP_RGB565_OES | BIT(6) | BIT(8));
    cpu_top_back ^= 1U;
}

void __wrap_C3Di_RenderQueueWaitDone(void) {
    if (starwing_gpu_abandoned() || !render_queue) return;
    const u64 deadline = svcGetSystemTick() + SYSCLOCK_ARM11 / 4U;
    while (!__wrap_gxCmdQueueWait(render_queue, 0)) {
        if (svcGetSystemTick() >= deadline) {
            starwing_abandon_gpu();
            return;
        }
        svcSleepThread(500000);
    }
    gxCmdQueueStop(render_queue);
    gxCmdQueueClear(render_queue);
}
