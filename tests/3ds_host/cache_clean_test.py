#!/usr/bin/env python3
"""Exercise the actual cache handoff helper with allowed and denied SVC 83."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
header = root / 'platform/3ds/source/cache_3ds.h'
mock = '''#pragma once
#include <stdint.h>
typedef uint32_t u32;
typedef int Result;
#define CUR_PROCESS_HANDLE 0xffff8001U
#define R_SUCCEEDED(r) ((r)>=0)
Result svcStoreProcessDataCache(u32,u32,u32);
Result GSPGPU_FlushDataCache(const void*,u32);
'''
source = r'''
#include <assert.h>
#include <stdio.h>
#include "cache_3ds.h"
static unsigned direct_calls,service_calls;
static int denied,failed;
Result svcStoreProcessDataCache(u32 handle,u32 address,u32 bytes) {
 assert(handle==CUR_PROCESS_HANDLE && address==0x14002000U && bytes==192U);
 ++direct_calls;return denied?-5:0;
}
Result GSPGPU_FlushDataCache(const void* address,u32 bytes) {
 assert((uintptr_t)address==0x14002000U && bytes==192U);
 ++service_calls;return failed?-7:0;
}
int main(void) {
 void* address=(void*)0x14002000U;
 assert(starwing_clean_device_cache(address,0)==0);
 assert(direct_calls==0 && service_calls==0);
 assert(starwing_clean_device_cache(address,192)==0);
 assert(direct_calls==1 && service_calls==0);
 denied=1;
 assert(starwing_clean_device_cache(address,192)==0);
 assert(direct_calls==2 && service_calls==1);
 assert(starwing_clean_device_cache(address,192)==0);
 assert(direct_calls==2 && service_calls==2);
 failed=1;
 assert(starwing_clean_device_cache(address,192)==-7);
 assert(direct_calls==2 && service_calls==3);
 puts("Cache clean: exact range, direct SVC, denied-loader fallback, cached fallback and error propagation PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='starwing-cache-clean-') as temporary:
    path=Path(temporary)
    (path/'3ds.h').write_text(mock)
    (path/'test.c').write_text(source)
    subprocess.run(['cc','-std=c11','-O2','-I'+str(path),'-I'+str(header.parent),str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
