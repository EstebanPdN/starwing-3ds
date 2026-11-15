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
        // HOME pending while the sound service does not answer: the loop must
        // still reach libctru's transition. It silences both rings and
        // quiesces the GPU without a CSND command or a completion wait.
        audio_test::home_pending=true;
        audio_test::csnd_completion_available=false;
        const auto waits=audio_test::csnd_completion_waits.load();
        audio_test::flushes.clear();
        std::atomic_bool entered{};
        std::thread home([&]{
            const bool running=hook.main_loop([&]{ ++quiesced; return true; });
            assert(running);
            static_cast<void>(running);
            entered=true;
        });
        const auto home_deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
        while(!entered && std::chrono::steady_clock::now()<home_deadline)
            std::this_thread::yield();
        const bool entered_without_completion=entered.load();
        audio_test::csnd_completion_available=true;
        home.join();
        audio_test::home_pending=false;
        assert(entered_without_completion && "a pending HOME waited for CSND");
        assert(quiesced==1);
        assert(audio_test::play_states.empty() && "a pending HOME issued CSND commands");
        assert(audio_test::csnd_completion_waits==waits);
        const auto silent=[](const std::int16_t* ring) {
            return std::all_of(ring,ring+audio_test::bytes/sizeof(std::int16_t),
                [](std::int16_t sample) { return sample==0; });
        };
        assert(silent(audio_test::left) && silent(audio_test::right));
        assert((audio_test::flushes==std::vector<std::uint64_t>{
            (std::uint64_t{1}<<63U)|audio_test::bytes,audio_test::bytes}));
        // Resuming stops the channels in ordinary context; the next tick
        // then starts a fresh ring.
        audio.stop_playback();
        assert((audio_test::play_states==std::vector<std::pair<int,int>>{{8,0},{9,0}}));
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
  puts("PASS: Old/New HOME and sleep hooks issue no CSND commands; a pending HOME silences the rings and quiesces the GPU with no CSND command, even when CSND does not answer");
}
