#include "native_engine/pica.hpp"
#include <citro2d.h>
#include <3ds.h>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace starfox;
int main() {
    gfxInitDefault();consoleInit(GFX_BOTTOM,nullptr);romfsInit();
    if(!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)||!C2D_Init(16384))return 1;
    C2D_Prepare();platform_3ds::native_engine::Pica engine;
    if(!engine.open()){std::printf("GPU OPEN FAILED\n");return 1;}
    auto* pixels=static_cast<std::uint32_t*>(linearAlloc(512*256*4));
    auto* listing=std::fopen("romfs:/cases.txt","rb");
    if(!pixels||!listing){std::printf("REPLAY INPUT/ALLOCATION FAILED\n");return 1;}
    unsigned cases{},failed{};unsigned long long differences{},tested{};int orientation=-1;char name[160]{};
    while(listing&&std::fgets(name,sizeof(name),listing)) {
        name[std::strcspn(name,"\r\n")]=0;char path[192]{};std::snprintf(path,sizeof(path),"romfs:/%s",name);
        auto* f=std::fopen(path,"rb");std::uint32_t count{};
        if(!f||std::fread(&count,4,1,f)!=1||count>16384){++failed;break;}
        std::vector<platform_3ds::native_engine::Rectangle> rectangles(count);
        render::Palette256 palette;std::vector<std::uint8_t> expected(400*240);
        bool read=std::fread(rectangles.data(),sizeof(rectangles[0]),count,f)==count
            &&std::fread(palette.data(),sizeof(palette),1,f)==1
            &&std::fread(expected.data(),expected.size(),1,f)==1;
        std::fclose(f);if(!read){++failed;break;}
        render::RasterCommands commands;commands.reset(400,240);
        for(const auto& r:rectangles){render::RasterCommand c{};c.left=r.left;c.top=r.top;c.right=r.right;c.bottom=r.bottom;
            c.even=r.even;c.odd=r.odd;c.dither=r.even!=r.odd;c.reserved0=r.scale;commands.commands.push_back(c);}
        platform_3ds::native_engine::Scene scene;
        if(!scene.compile(commands)||!C3D_FrameBegin(0)){++failed;break;}
        const bool drawn=engine.draw(scene,palette);C3D_FrameEnd(GX_CMDLIST_FLUSH);
        if(!drawn||!C3D_FrameBegin(0)){++failed;break;}C3D_FrameEnd(0);
        C3D_SyncDisplayTransfer(reinterpret_cast<u32*>(engine.texture()->data),GX_BUFFER_DIM(512,256),
            reinterpret_cast<u32*>(pixels),GX_BUFFER_DIM(512,256),GX_TRANSFER_FLIP_VERT(0)
            |GX_TRANSFER_OUT_TILED(0)|GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)|GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8));
        GSPGPU_InvalidateDataCache(pixels,512*256*4);
        unsigned delta[2]{};
        for(int flip=0;flip<2;++flip)for(unsigned y=0;y<240;++y)for(unsigned x=0;x<400;++x){
            auto p=pixels[(flip?255-y:y)*512+x];auto i=expected[y*400+x];const auto c=palette[i];
            const auto want=(u32(c.r)<<24)|(u32(c.g)<<16)|(u32(c.b)<<8)|255U;
            delta[flip]+=i?p!=want:(p&255U)!=0;
        }
        if(orientation<0)orientation=delta[1]<delta[0]?1:0;
        const auto error=delta[orientation];differences+=error;failed+=error!=0;tested+=400*240;++cases;
        std::printf("\x1b[0;0HSTARWING PICA REPLAY\n\nCASE %u  FAILED %u\nPIXELS %llu\nDIFFERENCES %llu\n\n%.35s\n",cases,failed,tested,differences,name);
    }
    if(listing)std::fclose(listing);
    std::printf("\nCOMPLETE  ORIENTATION %d\n",orientation);
    auto* report=std::fopen("sdmc:/native-gpu-results.json","wb");
    if(report){std::fprintf(report,"{\"cases\":%u,\"failed\":%u,\"pixels\":%llu,\"differences\":%llu,\"readback_vertical_flip\":%d}\n",cases,failed,tested,differences,orientation);std::fclose(report);}
    while(aptMainLoop()){hidScanInput();if(hidKeysDown()&KEY_START)break;gspWaitForVBlank();}
    if(C3D_FrameBegin(0))C3D_FrameEnd(0);engine.close();linearFree(pixels);C2D_Fini();C3D_Fini();romfsExit();gfxExit();
}
