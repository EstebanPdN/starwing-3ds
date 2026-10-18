#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>
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
    std::array<std::uint8_t, 4> output_ports() const { return {}; }
};
}
