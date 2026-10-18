#include "../platform/3ds/source/updater_3ds.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

using namespace starfox::platform_3ds;
namespace {
u64 title = update_title_id;
unsigned starts{}, commits{}, cancels{}, closes{}, exits{};
bool short_write{}, low_space{};
unsigned title_version=82;
std::vector<std::uint8_t> installed;
void require(bool value) { if (!value) std::abort(); }
void reset() { starts=commits=cancels=closes=exits=0; short_write=low_space=false; title=update_title_id; title_version=82; installed.clear(); }
void write(const std::string& path, const std::string& data) { std::ofstream(path, std::ios::binary) << data; }
std::string read(const std::string& path) {
    std::ifstream f(path, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}};
}
}
Result FSUSER_GetSdmcArchiveResource(FS_ArchiveResource* r) { *r={}; if(low_space) r->freeClusters=0; return 0; }
Result FSUSER_OpenFileDirectly(Handle* h, int, FS_Path, FS_Path, u32, u32) { *h=1; return 0; }
Result FSFILE_Read(Handle, u32* read, u64, void* p, u32 size) { std::memset(p, 0x5a, size); *read=size; return 0; }
Result FSFILE_Write(Handle, u32* wrote, u64, const void* p, u32 size, u32) {
    *wrote=short_write?size-1:size;
    auto* b=static_cast<const std::uint8_t*>(p); installed.insert(installed.end(),b,b+*wrote); return 0;
}
Result FSFILE_Close(Handle) { ++closes; return 0; }
Result amInit() { return 0; }
void amExit() { ++exits; }
Result AM_GetCiaFileInfo(FS_MediaType, AM_TitleInfo* p, Handle) { *p={title,4096,static_cast<std::uint16_t>(title_version)}; return 0; }
Result AM_GetCiaRequiredSpace(u64* p, FS_MediaType, Handle) { *p=4096; return 0; }
Result AM_StartCiaInstallOverwrite(Handle* h, FS_MediaType) { ++starts; *h=2; return 0; }
Result AM_CancelCIAInstall(Handle) { ++cancels; return 0; }
Result AM_FinishCiaInstall(Handle) { ++commits; return 0; }

namespace starfox::platform_3ds {
struct Updater3dsTest {
    static void seed(Updater3ds& u, unsigned bytes = 4096) { u.release_=UpdateRelease{"v1.1","",std::string(64,'a'),bytes,{}}; }
    static bool cia(Updater3ds& u) { return u.install_cia(); }
    static bool homebrew(Updater3ds& u) { return u.install_3dsx(); }
};
}
int main() {
    Updater3ds updater(""); Updater3dsTest::seed(updater);
    reset(); require(Updater3dsTest::cia(updater));
    require(starts==1 && commits==1 && cancels==0 && closes==1 && exits==1 && installed.size()==4096);
    reset(); title=update_title_id+1; require(!Updater3dsTest::cia(updater));
    require(starts==0 && commits==0 && closes==1 && exits==1);
    reset(); title_version=81; require(!Updater3dsTest::cia(updater)); require(starts==0);
    reset(); low_space=true; require(!Updater3dsTest::cia(updater)); require(starts==0);
    reset(); short_write=true; require(!Updater3dsTest::cia(updater));
    require(starts==1 && commits==0 && cancels==1 && closes==1 && exits==1);
    std::filesystem::create_directories("sdmc:/3ds/Starwing/update");
    updater.set_channel(true);
    require(updater.status().experimental);
    require(read("sdmc:/3ds/Starwing/update/channel.cfg")=="1");
    { Updater3ds restored(""); require(restored.status().experimental); }
    updater.set_channel(false);
    require(!updater.status().experimental && updater.status().version[0]==0);
    { Updater3ds restored(""); require(!restored.status().experimental); }
    const std::string launch="sdmc:/3ds/Starwing/Starwing-3DS.3dsx";
    Updater3ds homebrew(launch.c_str());
    write(launch,"old"); write("sdmc:/3ds/Starwing/update/download.part","new");
    require(Updater3dsTest::homebrew(homebrew));
    require(read(launch)=="new" && read(launch+".bak-1.0")=="old");
    write("sdmc:/3ds/Starwing/update/download.part","newer");
    require(!Updater3dsTest::homebrew(homebrew)); require(read(launch)=="new");
    std::filesystem::remove(launch+".bak-1.0");
    std::filesystem::remove("sdmc:/3ds/Starwing/update/download.part");
    require(!Updater3dsTest::homebrew(homebrew)); require(read(launch)=="new");
    std::cout << "PASS: CIA overwrite, identity refusal, space refusal, partial-write cancellation, 3DSX backup and rollback\n";
}
