#include "save_storage_3ds.hpp"
#include "audio_3ds.hpp"
#include "starfox/assets/rom.hpp"
#include "starfox/simulation/game_simulation.hpp"
#include "starfox/state/archive.hpp"
#include "starfox/state/container.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <string>

namespace starfox::platform_3ds {
namespace {
constexpr std::uint32_t autosave_schema = 0x41555301U;
}

bool write_autosave(const char* path,const starfox::assets::RomImage& rom,
    const starfox::simulation::GameSimulation& game,
    const starfox::platform_3ds::Audio3ds& audio) {
    starfox::state::Writer writer;
    writer(game.save_state(),audio.spc().save_state());
    const auto bytes=starfox::state::pack(autosave_schema,
        rom.crc32(),writer.bytes());
    const std::string temporary=std::string(path)+".tmp";
    FILE* file=std::fopen(temporary.c_str(),"wb");
    if(!file) return false;
    const bool written=std::fwrite(bytes.data(),1,bytes.size(),file)
        ==bytes.size();
    const bool closed=std::fclose(file)==0;
    if(written&&closed&&std::rename(temporary.c_str(),path)==0) return true;
    std::remove(temporary.c_str());
    return false;
}

bool restore_autosave(const char* path,const starfox::assets::RomImage& rom,
    starfox::simulation::GameSimulation& game,
    starfox::platform_3ds::Audio3ds& audio) {
    FILE* file=std::fopen(path,"rb");
    if(!file) return false;
    const bool size_ok=std::fseek(file,0,SEEK_END)==0;
    const long length=size_ok?std::ftell(file):-1;
    if(length<=0||length>16L*1024L*1024L
        ||std::fseek(file,0,SEEK_SET)!=0) {
        std::fclose(file);return false;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    const bool read=std::fread(bytes.data(),1,bytes.size(),file)
        ==bytes.size();
    std::fclose(file);
    if(!read) return false;
    try {
        starfox::state::Reader reader{starfox::state::unpack(bytes,
            autosave_schema,rom.crc32())};
        std::vector<std::uint8_t> game_bytes,audio_bytes;
        reader(game_bytes,audio_bytes);
        reader.finish();
        auto restored=game.restored_state(game_bytes);
        audio.spc().load_state(audio_bytes);
        game.swap_state(*restored);
        game.synchronize_apu_output_ports(audio.output_ports());
        return true;
    } catch(const std::exception&) {
        return false;
    }
}

std::vector<std::uint8_t> read_ex_save(const char* path) {
    std::vector<std::uint8_t> save(65'536U);
    FILE *file = std::fopen(path, "rb");
    if (file != nullptr) {
        if (std::fread(save.data(), 1, save.size(), file) != save.size())
            std::fill(save.begin(), save.end(), 0U);
        std::fclose(file);
    }
    return save;
}

void write_ex_save(std::span<const std::uint8_t> bytes,const char* path) {
    if (bytes.size() != 65'536U) return;
    const std::string temporary = std::string(path) + ".tmp";
    FILE *file = std::fopen(temporary.c_str(), "wb");
    if (file == nullptr) return;
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file)
        == bytes.size();
    const bool closed = std::fclose(file) == 0;
    if (written && closed) std::rename(temporary.c_str(), path);
}

} // namespace starfox::platform_3ds
