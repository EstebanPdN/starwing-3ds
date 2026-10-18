#include "audio_3ds.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

// Run this against both versions of the real Audio3ds implementation and
// compare the transcripts byte-for-byte. Only libctru services are faked.
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ofstream out{argv[1], std::ios::binary};
    auto record = [&](const auto& value) {
        out.write(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    starfox::platform_3ds::Audio3ds audio;
    if (!audio.open()) return 1;
    std::array<std::int16_t, 3200> samples;
    std::uint32_t rng = 0x6832856U;
    for (unsigned tick = 0; tick < 4096; ++tick) {
        for (auto& sample : samples) {
            rng = rng * 1664525U + 1013904223U;
            sample = static_cast<std::int16_t>(rng >> 16U);
        }
        // Sustained playback, wrapping, starvation, source bursts, controls,
        // long stalls, stop/restart, volume extremes and service failures.
        audio_test::now_ms += tick % 73 == 0 ? 997 : tick % 41 < 10 ? 0 : 50;
        if (tick % 257 == 0) audio.stop_playback();
        if (tick % 199 == 0) audio_test::now_ms += (1ULL << 33);
        audio.set_master_volume(tick % 127);
        audio_test::flush_fail_after = tick % 317 == 0 ? 1 : -1;
        audio_test::play_result = tick % 514 == 0 ? 1 : 0;
        audio_test::flushes.clear();
        record(audio.push_samples(samples));
        if (audio_test::left != nullptr) {
            out.write(reinterpret_cast<const char*>(audio_test::left), audio_test::bytes);
            out.write(reinterpret_cast<const char*>(audio_test::right), audio_test::bytes);
        }
        for (auto flush : audio_test::flushes) record(flush);
        record(audio_test::flushes.size());
        const auto& d = audio.diagnostics();
        record(d.submitted_packets); record(d.late_packets); record(d.late_frames);
        record(d.dropped_packets); record(d.playback_starts);
        record(d.cache_flush_failures); record(d.max_lead_frames);
        record(d.rate_adjust_frames); record(d.playback_start_result);
    }
    if (!out.good()) return 1;
    const auto& d = audio.diagnostics();
    std::cout << "4096 ring updates: submitted=" << d.submitted_packets
        << " late=" << d.late_packets << " dropped=" << d.dropped_packets
        << " starts=" << d.playback_starts << " flush_failures="
        << d.cache_flush_failures << '\n';
}
