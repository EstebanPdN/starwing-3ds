#!/usr/bin/env python3
"""Run the actual private Citro2D frame-end hook against mocked GSP calls."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
base = (root / "platform/3ds/vendor/citro2d/source/base.c").read_text()
start = base.index("static size_t frameCacheFlushBytes;")
end = base.index("\nbool C2D_Init", start)
hook = base[start:end]
internal = (root / "platform/3ds/vendor/citro2d/source/internal.h").read_text()
vertex = internal[internal.index("typedef struct"):internal.index("} C2Di_Vertex;") + len("} C2Di_Vertex;")]
mock = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef uint16_t u16;
typedef uint32_t u32;
typedef int Result;
#define R_FAILED(r) ((r)<0)
VERTEX
typedef struct { C2Di_Vertex* vtxBuf; u16* idxBuf; size_t vtxBufPos,idxBufPos,idxBufLastPos; } C2Di_Context;
static C2Di_Context context;
static unsigned call_count, vertex_flush_count, fail_call, draw_flushed;
static void* addresses[3];
static size_t sizes[3];
u32 __ctru_linear_heap=0x14000000U, __ctru_linear_heap_size=32U*1024U*1024U;
static C2Di_Context* C2Di_GetContext(void) { return &context; }
static void C2Di_FlushVtxBuf(void) { ++draw_flushed; assert(context.vtxBufPos==vertex_flush_count); }
static Result GSPGPU_FlushDataCache(void* ptr,size_t bytes) {
    assert(draw_flushed==1); assert(context.vtxBufPos==vertex_flush_count);
    assert(call_count<3);addresses[call_count]=ptr;sizes[call_count]=bytes;++call_count;
    return call_count==fail_call?-1:0;
}
HOOK
int main(void) {
    _Static_assert(sizeof(C2Di_Vertex)==32,"actual GPU vertex stride changed");
    static C2Di_Vertex vertices[65536];static u16 indices[98304];
    for(unsigned pass=0;pass<4;++pass) {
        const unsigned objects=pass==0?0:pass==1?1:16384;
        context=(C2Di_Context){vertices,indices,objects*4,objects*6,objects*6};
        vertex_flush_count=context.vtxBufPos;call_count=draw_flushed=0;fail_call=pass==3?2:0;
        C2Di_FrameEndHook(NULL);
        assert(context.vtxBufPos==0 && context.idxBufPos==0 && context.idxBufLastPos==0);
        assert(call_count==(pass==0?0:pass==3?3:2));
        if(objects) { assert(addresses[0]==vertices && sizes[0]==objects*4*sizeof(C2Di_Vertex));
            assert(addresses[1]==indices && sizes[1]==objects*6*sizeof(u16)); }
        assert(C2D_GetFrameCacheFlushBytes()==(pass==3?__ctru_linear_heap_size:objects*140U));
        assert(C2D_GetFrameCacheFlushFallbacks()==(pass==3?1U:0U));
        if(pass==3) {assert((uintptr_t)addresses[2]==__ctru_linear_heap);assert(sizes[2]==__ctru_linear_heap_size);}
    }
    puts("Citro2D actual hook: empty, mono, full-capacity stereo, used-range order and failure fallback passed");
}
'''.replace("VERTEX", vertex).replace("HOOK", hook)
with tempfile.TemporaryDirectory(prefix="starwing-cache-test-") as directory:
    source = Path(directory)/"test.c"
    executable = Path(directory)/"test"
    source.write_text(mock)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2",
                    "-fsanitize=address,undefined", str(source), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
