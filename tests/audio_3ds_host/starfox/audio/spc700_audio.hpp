#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>
#ifdef STARWING_AUDIO_REAL_THREADS
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <stdexcept>
namespace audio_test {
inline std::mutex dsp_mutex;
inline std::condition_variable dsp_changed;
inline bool release_dsp{};
inline unsigned fail_at{};
inline std::array<std::uint8_t,4> ports{};
}
#endif
namespace starfox::simulation {
struct ApuPortWrite { std::uint8_t port, value; std::uint32_t clock_offset; };
}
namespace starfox::audio {
// The ring test invokes push_samples directly; synthesis is deliberately fake.
class Spc700Audio {
public:
    enum class Routing { split_stems, cartridge };
    Spc700Audio() = default;
    explicit Spc700Audio(Routing value) : routing_(value) {}
    Routing routing() const { return routing_; }
    Routing routing_{Routing::split_stems};
    static constexpr unsigned sample_rate = 32000;
    static constexpr unsigned stereo_frames_per_logic_tick = 1600;
    void set_parallel_stems_enabled(bool) {}
    std::size_t prime_upload_sequence(std::span<const simulation::ApuPortWrite>) { return 0; }
    std::vector<std::int16_t> render_logic_tick(std::span<const simulation::ApuPortWrite>) { return {}; }
    void render_logic_tick(std::span<const simulation::ApuPortWrite>, std::vector<std::int16_t>& out) { out.clear(); }
    using OutputPortsReady = void (*)(void*,const std::array<std::uint8_t,4>&) noexcept;
    void render_logic_tick_with_ports(std::span<const simulation::ApuPortWrite>,
        std::vector<std::int16_t>& out, OutputPortsReady ready, void* context) {
#ifdef STARWING_AUDIO_REAL_THREADS
        if (audio_test::fail_at==1) throw std::runtime_error("CPU failure");
        ready(context,audio_test::ports);
        std::unique_lock lock(audio_test::dsp_mutex);
        audio_test::dsp_changed.wait(lock,[]{return audio_test::release_dsp;});
        if (audio_test::fail_at==2) throw std::runtime_error("DSP failure");
        out.assign(3200,17);
#else
        out.clear();ready(context,{});
#endif
    }
    std::array<std::uint8_t, 4> output_ports() const {
#ifdef STARWING_AUDIO_REAL_THREADS
        return audio_test::ports;
#else
        return {};
#endif
    }
};
}
