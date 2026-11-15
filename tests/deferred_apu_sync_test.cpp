#include "../platform/3ds/source/deferred_apu_sync.hpp"
#include "starfox/assets/rom.hpp"
#include "starfox/simulation/wdc65816.hpp"
#include <array>
#include <stdexcept>

struct Map {
    void (*fence)(void*) noexcept{};void* context{};
    std::array<unsigned char,4> ports{};
    void set_apu_access_fence(void (*callback)(void*) noexcept,void* value) {
        fence=callback;context=value;
    }
    void set_apu_output_ports(const std::array<unsigned char,4>& value){ports=value;}
    void access() {auto f=fence;fence=nullptr;if(f)f(context);}
};
struct Audio {
    bool outstanding=true,failed=false;unsigned calls=0;
    bool pending()const{return outstanding;}
    std::array<unsigned char,4> wait_for_output_ports(){++calls;if(failed)throw std::runtime_error("SPC failure");return output_ports();}
    std::array<unsigned char,4> output_ports()const{return {1,2,3,4};}
};
void require(bool condition){if(!condition)throw std::runtime_error("deferred APU assertion");}
int main() {
    Map map;Audio audio;
    {
        starfox::platform_3ds::DeferredApuSync sync{map,audio};
        require(audio.calls==0 && map.ports[0]==0);
        map.access();require(audio.calls==1 && map.ports==audio.output_ports());
        require(audio.outstanding); // PCM may still be rendering after the fence.
        map.access();require(audio.calls==1);sync.check_failure();
    }
    require(map.fence==nullptr);
    audio.outstanding=true;
    {starfox::platform_3ds::DeferredApuSync sync{map,audio};}
    require(audio.calls==1 && audio.outstanding && map.fence==nullptr);
    audio.failed=true;
    {
        starfox::platform_3ds::DeferredApuSync sync{map,audio};
        map.access(); // The noexcept memory API must not terminate on SPC error.
        bool reported=false;
        try{sync.check_failure();}catch(const std::runtime_error&){reported=true;}
        require(reported && audio.calls==2);
    }
    require(map.fence==nullptr);
    // Verify the actual bus also fences writes before an IPL bank restart.
    starfox::assets::RomImage rom(std::vector<std::uint8_t>(32768,0xea));
    starfox::simulation::Wdc65816 cpu(rom);
    Audio real_audio;
    {
        starfox::platform_3ds::DeferredApuSync sync{cpu,real_audio};
        cpu.write8(0x7e0010,0x77);
        require(real_audio.calls==0);
        require(cpu.read8(0x002140)==1 && real_audio.calls==1);
        require(cpu.read8(0x802143)==4 && real_audio.calls==1);
    }
    // Normal input writes need no response from the outstanding packet.
    real_audio.outstanding=true;
    {
        starfox::platform_3ds::DeferredApuSync sync{cpu,real_audio};
        cpu.write8(0x002141,0x55);
        cpu.write8(0x802142,0x66);
        require(real_audio.calls==1);
        require(cpu.read8(0x002142)==3 && real_audio.calls==2);
        const auto writes=cpu.take_apu_port_writes();
        require(writes.size()==2 && writes[0].value==0x55 && writes[1].value==0x66);
    }
    real_audio.outstanding=true;
    {
        starfox::platform_3ds::DeferredApuSync sync{cpu,real_audio};
        cpu.write8(0x002140,0xff);
        require(real_audio.calls==3);
        require(cpu.read8(0x002140)==0xaa && cpu.read8(0x002141)==0xbb);
    }
}
