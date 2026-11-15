// ROM-free device check using the exact self-test shipped in the CIA.
#include "display_3ds.hpp"
#include "gpu_self_test.hpp"
#include <memory>
#include <sys/stat.h>
#ifndef STARWING_GPU_TEST_OUTPUT_DIR
#define STARWING_GPU_TEST_OUTPUT_DIR "sdmc:/display-gpu-test"
#endif
extern "C" {
std::uint32_t __ctru_linear_heap_size=16U*1024U*1024U;
std::uint32_t __stacksize__=512U*1024U;
}
int main() {
    gfxInitDefault(); consoleInit(GFX_TOP,nullptr);
    auto display=std::make_unique<starfox::platform_3ds::Display3ds>();
    if(!display->open() || !display->open_native_engine()) return 1;
    mkdir(STARWING_GPU_TEST_OUTPUT_DIR,0777);
    starfox::platform_3ds::run_gpu_self_test(*display,STARWING_GPU_TEST_OUTPUT_DIR,true);
    while(aptMainLoop()) {
        hidScanInput(); if(hidKeysDown()&KEY_START) break; gspWaitForVBlank();
    }
    display.reset();gfxExit();return 0;
}
