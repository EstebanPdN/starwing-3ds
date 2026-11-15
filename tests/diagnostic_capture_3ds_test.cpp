// Exercise the actual capture() path while image-sized C++ allocations fail.
#include "diagnostics_3ds.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <new>

static bool restricted=false;
static std::size_t largest_request=0;
void* operator new(std::size_t n) {
    if (restricted) {
        largest_request=std::max(largest_request,n);
        if (n>=65536U) throw std::bad_alloc{};
    }
    if (auto* p=std::malloc(n?n:1)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
extern "C" {std::uint32_t __ctru_linear_heap_size=16U*1024U*1024U;}
int main() {
    gfxInitDefault();
    gfxSetScreenFormat(GFX_TOP,GSP_RGB565_OES);
    gfxSetScreenFormat(GFX_BOTTOM,GSP_RGBA8_OES);
    for (auto screen:{GFX_TOP,GFX_BOTTOM}) {
        u16 w,h;auto* fb=gfxGetFramebuffer(screen,GFX_LEFT,&w,&h);
        const unsigned bpp=screen==GFX_TOP?2U:4U;
        for (unsigned i=0;i<unsigned(w)*h*bpp;++i) fb[i]=std::uint8_t(i*13U);
    }
    starfox::platform_3ds::Diagnostics3ds diagnostics;
    starfox::platform_3ds::DiagnosticReport3ds report;
    report.build="Prueba 7 allocation fault test";
    report.framebuffer_snapshots=true;
    unsigned completed=0;
    restricted=true;
    try {
        for (unsigned i=0;i<8;++i) {
            const auto result=diagnostics.capture(starfox::platform_3ds::DiagnosticKind3ds::quick,report);
            if (!result.ok) break;
            ++completed;
        }
    } catch (const std::bad_alloc&) {}
    restricted=false;
    auto* file=std::fopen("sdmc:/diagnostic-capture-result.json","wb");
    if (file) {
        std::fprintf(file,"{\"completed\":%u,\"expected\":8,\"allocation_limit_bytes\":65536,\"largest_request_bytes\":%u}\n",
            completed,unsigned(largest_request));
        std::fclose(file);
    }
    while (aptMainLoop()) {hidScanInput();if (hidKeysDown()&KEY_START) break;gspWaitForVBlank();}
    gfxExit();return completed==8?0:1;
}
