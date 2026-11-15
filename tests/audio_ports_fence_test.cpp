#include "audio_3ds.hpp"
#include <cstdio>
#include <stdexcept>
using namespace starfox::platform_3ds;
static void require(bool value,const char* message) {
    if (!value) throw std::runtime_error(message);
}
static void release() {
    {std::lock_guard lock(audio_test::dsp_mutex);audio_test::release_dsp=true;}
    audio_test::dsp_changed.notify_one();
}
struct ReleaseOnExit { ~ReleaseOnExit(){release();} };
int main() try {
    audio_test::honour_quota=audio_test::thread_succeeds=true;
    Audio3ds audio; require(audio.open(),"Audio open failed");
    ReleaseOnExit unblock;
    for (unsigned packet=0;packet<40;++packet) {
        audio_test::fail_at=packet==35?1:packet==36?2:0;
        audio_test::ports={std::uint8_t(packet),11,22,33};
        {std::lock_guard lock(audio_test::dsp_mutex);audio_test::release_dsp=false;}
        audio.begin_update({});
        bool ports_failed=false;
        if (packet%3!=0 || audio_test::fail_at) {
            try {
                require(audio.wait_for_output_ports()==audio_test::ports,"Wrong early ports");
                require(audio.pending() && !audio.update_ready(),"Ports waited for unfinished DSP");
                require(audio.output_ports()==audio_test::ports,"Pending port read accessed live SPC");
                require(audio.wait_for_output_ports()==audio_test::ports,"Second read consumed another event");
            } catch(const std::runtime_error& error) {
                if (audio_test::fail_at!=1) throw;
                ports_failed=true;
            }
        }
        require(ports_failed==(audio_test::fail_at==1),"CPU error did not wake the port fence");
        release();
        bool finish_failed=false;
        try { (void)audio.finish_update(); }
        catch(const std::runtime_error&) {finish_failed=true;}
        require(finish_failed==(audio_test::fail_at!=0),"Worker error was lost");
        require(!audio.pending(),"Completed packet stayed pending after failure");
    }
    std::puts("PASS 40 threaded packets: early ports, outstanding DSP, unconsumed events, CPU/DSP failures and recovery");
} catch(const std::exception& error) {std::fprintf(stderr,"%s\n",error.what());return 1;}
