#include "diagnostic_png.hpp"
#include <cstdlib>
#include <new>
#include <stdexcept>

static bool refuse_allocations=false;
void* operator new(std::size_t bytes) {
    if (refuse_allocations) throw std::bad_alloc{};
    if (auto* p=std::malloc(bytes?bytes:1)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
int main(int argc,char** argv) {
    if (argc!=2) return 1;
    const auto pixel=[](unsigned x,unsigned y) {
        return std::array<std::uint8_t,3>{std::uint8_t(x*17+y*3),
            std::uint8_t(x*7+y*29),std::uint8_t(x^y)};
    };
    const auto write=[&](unsigned width,unsigned height) {
        char path[1024];std::snprintf(path,sizeof(path),"%s/%ux%u.png",argv[1],width,height);
        auto* file=std::fopen(path,"wb");if (!file) return false;
        refuse_allocations=true;
        const bool ok=starfox::platform_3ds::write_diagnostic_rgb_png(file,width,height,pixel);
        refuse_allocations=false;
        return std::fclose(file)==0 && ok;
    };
    if (!write(400,240) || !write(320,240) || !write(400,1) || !write(1,1) || !write(13,37)) return 2;
    char path[1024];std::snprintf(path,sizeof(path),"%s/400x240.png",argv[1]);
    auto* read_only=std::fopen(path,"rb");if (!read_only) return 3;
    if (starfox::platform_3ds::write_diagnostic_rgb_png(read_only,400,240,pixel)) return 4;
    std::fclose(read_only);
    if (starfox::platform_3ds::write_diagnostic_rgb_png(nullptr,400,240,pixel)) return 5;
    std::puts("PASS: PNG streaming with every C++ allocation refused, row boundaries and I/O errors");
}
