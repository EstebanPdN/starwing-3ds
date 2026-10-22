#include "apt_audio_3ds.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace starfox::platform_3ds;

// HOME/sleep hooks run inside libctru's transition and must not touch CSND.
// A pending HOME press silences the ring in ordinary context first.
int main() {
  for (const bool new_model : {false,true}) {
    audio_test::new_3ds=new_model;
    Audio3ds audio;
    assert(audio.open());
    std::array<std::int16_t,3200> samples;
    samples.fill(12345);
    {
        AptAudioPause hook{audio};
        assert(audio_test::apt_callback);
        for(const auto event:{APTHOOK_ONSUSPEND,APTHOOK_ONSLEEP}) {
            assert(audio.push_samples(samples));
            audio_test::play_states.clear();
            audio_test::csnd_completion_available=false;
            std::atomic_bool returned{};
            std::thread transition([&]{
                audio_test::apt_callback(event,audio_test::apt_param);
                returned=true;
            });
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
            while(!returned && std::chrono::steady_clock::now()<deadline)
                std::this_thread::yield();
            const bool returned_without_completion=returned.load();
            audio_test::csnd_completion_available=true;
            transition.join();
            assert(returned_without_completion && "HOME/sleep hook waited for CSND");
            assert(audio_test::play_states.empty() && "the transition hook issued CSND commands");
            assert(!hook.take_resumed());
            audio_test::apt_callback(event==APTHOOK_ONSUSPEND?APTHOOK_ONRESTORE:APTHOOK_ONWAKEUP,
                audio_test::apt_param);
            assert(hook.take_resumed() && !hook.take_resumed());
            audio.stop_playback();
        }
        // No HOME press: the loop neither stops audio nor quiesces the GPU.
        assert(audio.push_samples(samples));
        audio_test::play_states.clear();
        unsigned quiesced=0;
        assert(hook.main_loop([&]{ ++quiesced; return true; }));
        assert(quiesced==0 && audio_test::play_states.empty());
        // HOME pending: stop and quiesce before libctru starts the transition.
        audio_test::home_pending=true;
        assert(hook.main_loop([&]{ ++quiesced; return true; }));
        audio_test::home_pending=false;
        assert(quiesced==1);
        assert((audio_test::play_states==std::vector<std::pair<int,int>>{{8,0},{9,0}}));
        assert(audio_test::play_states_at_main_loop==2 && "audio stopped after the transition began");
        const auto starts=audio.diagnostics().playback_starts;
        audio_test::now_ms+=60000;
        assert(audio.push_samples(samples));
        assert(audio.diagnostics().playback_starts==starts+1);
        audio.stop_playback();
    }
    assert(!audio_test::apt_callback);
    audio_test::left=audio_test::right=nullptr;
    audio_test::bytes=0;
  }
  puts("PASS: Old/New HOME and sleep hooks issue no CSND commands; a pending HOME stops audio and quiesces the GPU before the transition");
}
