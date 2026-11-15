#pragma once

#include "starfox/simulation/wdc65816.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <span>
#include <vector>

// SPC700/S-DSP audio for the engine. The 3DS audio backend (Audio3ds) and host
// tests feed it each 20 Hz tick's CPU-to-APU port writes and receive 32 kHz
// stereo PCM plus the output ports the game reads back.
namespace starfox::audio {

// Runs the cartridge's uploaded SPC700 driver and BRR sample data through
// the cycle-accurate SNES_SPC/S-DSP core. The cartridge upload protocol is
// decoded into its post-IPL state so Nintendo's separate 64-byte IPL ROM is
// neither required nor embedded.
class Spc700Audio {
public:
    // Diagnostic snapshot of the music (or only) SPC: CPU registers, the DSP
    // FLG, KON and main volume registers, and output_ports(). All zero until
    // a driver is loaded.
    struct State {
        std::uint16_t program_counter{};
        std::uint8_t accumulator{};
        std::uint8_t x{};
        std::uint8_t y{};
        std::uint8_t status{};
        std::uint8_t stack{};
        std::uint8_t dsp_flags{};
        std::uint8_t dsp_key_on{};
        std::int8_t main_volume_left{};
        std::int8_t main_volume_right{};
        std::array<std::uint8_t, 4> output_ports{};
    };

    // S-DSP output rate in Hz, and stereo frames per 20 Hz logic tick (50 ms).
    static constexpr std::uint32_t sample_rate = 32'000;
    static constexpr std::size_t stereo_frames_per_logic_tick = 1'600;

    // cartridge runs one SPC, so music and effects share the original eight
    // DSP voices. split_stems runs music (port 0) and effects (ports 1-3) on
    // two SPC instances and mixes them. The 3DS frontend selects cartridge on
    // Old 3DS (or an unknown model) and split_stems on New 3DS.
    enum class Routing { split_stems, cartridge };
    // The default routing is split_stems. Throws std::runtime_error if an SPC
    // instance cannot be allocated.
    Spc700Audio();
    explicit Spc700Audio(Routing routing);
    [[nodiscard]] Routing routing() const noexcept { return routing_; }
    // Driver code blocks the SPC core executed natively instead of
    // interpreting them, summed over both stems; a profiling counter.
    [[nodiscard]] std::uint64_t native_blocks_executed() const noexcept;
    ~Spc700Audio();
    Spc700Audio(Spc700Audio&&) noexcept;
    Spc700Audio& operator=(Spc700Audio&&) noexcept;
    Spc700Audio(const Spc700Audio&) = delete;
    Spc700Audio& operator=(const Spc700Audio&) = delete;

    // Applies CPU-to-APU writes from one 20 Hz gameplay update and returns
    // exactly 50 ms of native 32 kHz interleaved stereo output. Split stems
    // are summed with 16-bit saturation.
    [[nodiscard]] std::vector<std::int16_t> render_logic_tick(
        std::span<const simulation::ApuPortWrite> writes);

    // Reuse the caller's PCM allocation on real-time backends. After the
    // first packet, rendering and mixing need no packet-buffer allocation.
    // Samples, port timing and serialized state match the value-return API.
    void render_logic_tick(std::span<const simulation::ApuPortWrite> writes,
        std::vector<std::int16_t>& output);

    using OutputPortsReady = void (*)(void*, const std::array<std::uint8_t, 4>&) noexcept;
    // Publish the final CPU-facing ports as soon as the cartridge SPC has
    // executed the tick. Its remaining DSP catch-up, filter and PCM copy do
    // not affect these ports and may finish while the game resumes its tick.
    // The callback runs on the caller's thread, once, and must not access this
    // object. Split stems publish after both cores finish.
    void render_logic_tick_with_ports(std::span<const simulation::ApuPortWrite> writes,
        std::vector<std::int16_t>& output, OutputPortsReady callback, void* context);

    // BGM commands (CPU port 0) and effect commands (continuous engine and
    // positional audio on ports 1/2 plus queued effects on port 3) run on
    // independent SPC700 instances. Exposing the most recent stems makes it
    // possible to verify that an effect changed only the effect mix and never
    // consumed or muted a music voice.
    [[nodiscard]] std::span<const std::int16_t> last_music_samples() const
        noexcept { return last_music_samples_; }
    // Empty in cartridge mode; last_music_samples contains the complete mix.
    [[nodiscard]] std::span<const std::int16_t> last_effect_samples() const
        noexcept { return last_effect_samples_; }

    // Services all complete IPL uploads captured during synchronous cartridge
    // construction. If one captured stream contains multiple banks, each
    // replacement is separated by the driver's $ff restart command and must
    // receive its own 50 ms SPC frame so the preceding driver can initialize
    // the ARAM workspace before the next bank overlays it.
    // Returns the number of SPC frames rendered per stem and leaves both stem
    // sample buffers empty.
    [[nodiscard]] std::size_t prime_upload_sequence(
        std::span<const simulation::ApuPortWrite> writes);

    // True when every SPC instance runs an uploaded driver; false until the
    // first upload completes and while a bank is being uploaded.
    [[nodiscard]] bool driver_loaded() const noexcept;
    // Data bytes received by the current or last upload; with split stems,
    // the smaller of the two counts.
    [[nodiscard]] std::size_t uploaded_bytes() const noexcept;
    // Ports the CPU reads: port 0 from the music stem and ports 1-3 from the
    // effects stem (all four from the one cartridge SPC). Zero until a driver
    // is loaded.
    [[nodiscard]] std::array<std::uint8_t, 4> output_ports() const noexcept;
    [[nodiscard]] State state() const;
    // save_state packs the SPC core state, upload decoders, filters and last
    // stem samples into a state::pack envelope. load_state accepts either
    // routing's layout and adopts the routing it stores; it throws
    // std::runtime_error for malformed data and leaves this object unchanged.
    [[nodiscard]] std::vector<std::uint8_t> save_state() const;
    void load_state(std::span<const std::uint8_t> bytes);
#ifdef __3DS__
    // Audio3ds can run a whole SPC tick on core 1. In that mode its worker
    // owns the only application thread allowed there, so keep both stems on
    // that worker rather than starting the stem worker as well.
    // Enabled by default; disabling destroys the stem worker.
    void set_parallel_stems_enabled(bool enabled) noexcept;
#endif

private:
    struct Impl;
    void render_stems(std::span<const simulation::ApuPortWrite> writes,
        bool split_upload_restarts, std::size_t* music_frames,
        std::size_t* effect_frames);
#ifdef __3DS__
    struct ParallelRenderer;
#endif
    Routing routing_{Routing::split_stems};
    std::unique_ptr<Impl> music_impl_;
    std::unique_ptr<Impl> effects_impl_;
    std::vector<std::int16_t> last_music_samples_;
    std::vector<std::int16_t> last_effect_samples_;
#ifdef __3DS__
    std::unique_ptr<ParallelRenderer> parallel_;
    bool parallel_stems_enabled_{true};
#endif
};

} // namespace starfox::audio
