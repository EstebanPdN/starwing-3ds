#pragma once

#include "starfox/assets/rom.hpp"
#include "starfox/simulation/math.hpp"
#include "starfox/simulation/object_pool.hpp"
#include "starfox/simulation/prng.hpp"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

// Host reference model of the PATH bytecode interpreter, used only by host
// tests. The game runs the cartridge's own PATH code on the 65C816 core
// through NativeStrategyScheduler.

namespace starfox::simulation {

enum class PathEventKind : std::uint8_t {
    message,
    sound_effect,
    positional_sound,
    fire_weapon,
};

// A side effect the model records instead of performing. `value` is the
// message number, the sound number or the firing object's weapon type.
struct PathEvent {
    PathEventKind kind{};
    ObjectHandle source{};
    std::uint16_t value{};
};

// Interpreter for the compact PATH stream emitted by PATHMACS.ASM. Path
// offsets are the original 16-bit offsets from the ROM's `paths` symbol.
// Keeps pointers to `rom`, `objects` and `random`, which must outlive it.
class PathVm {
public:
    // Explicit addresses; opcodes keep the retail numbering.
    PathVm(
        const assets::RomImage& rom,
        std::uint32_t paths_address,
        std::uint32_t path_strategy_address,
        ObjectPool& objects,
        TrigTables trig,
        OriginalPrng& random,
        std::uint32_t particle_strategy_address = 0,
        std::uint16_t null_shape = 0) noexcept;
    // Resolves PATHS, PATH_ISTRAT, PARTICLEEXPLODE_ISTRAT, NULLSHAPE and the
    // sine tables; throws std::runtime_error when one is missing. On a Star
    // Fox EX ROM opcodes are renumbered from its exported P_* constants, and
    // EX-only opcodes are unsupported.
    PathVm(
        const assets::RomImage& rom,
        const assets::SymbolMap& symbols,
        ObjectPool& objects,
        OriginalPrng& random);

    // Throws std::invalid_argument for an inactive handle.
    void set_player(ObjectHandle player);

    // Gives an active object the PATH strategy, starting at `path_offset`,
    // with fresh interpreter state; throws std::invalid_argument for an
    // inactive handle.
    void attach(ObjectHandle object, std::uint16_t path_offset);
    // Runs the object's PATH instructions until one ends its update (most of
    // those move the object once). Throws std::runtime_error with no active
    // player, on a stack underflow, an unsupported opcode (also recorded in
    // unsupported_opcodes()) or after 65,536 instructions.
    void tick(ObjectHandle object);
    // tick() for every attached object, in the active-list order taken at
    // the call.
    void tick_all();

    // The model's bank $7e copy that P_IMPORT/P_EXPORT read and write.
    [[nodiscard]] std::uint8_t read_global_byte(
        std::uint16_t address) const noexcept {
        return global_memory_[address];
    }
    [[nodiscard]] std::uint16_t read_global_word(
        std::uint16_t address) const noexcept {
        return static_cast<std::uint16_t>(global_memory_[address])
            | (static_cast<std::uint16_t>(global_memory_[
                   static_cast<std::uint16_t>(address + 1U)]) << 8U);
    }
    void write_global_byte(
        std::uint16_t address, std::uint8_t value) noexcept {
        global_memory_[address] = value;
    }
    void write_global_word(
        std::uint16_t address, std::uint16_t value) noexcept {
        global_memory_[address] = static_cast<std::uint8_t>(value);
        global_memory_[static_cast<std::uint16_t>(address + 1U)] =
            static_cast<std::uint8_t>(value >> 8U);
    }
    // Total added by P_SCORE, also stored at PLAYERSCORE in the bank copy
    // when that symbol exists.
    [[nodiscard]] std::uint16_t player_score() const noexcept {
        return player_score_;
    }

    // Whether the object is active and runs the PATH strategy.
    [[nodiscard]] bool is_attached(ObjectHandle object) const noexcept;
    // The object's current offset (kept in scratch_words[1]); throws
    // std::out_of_range for an inactive handle.
    [[nodiscard]] std::uint16_t path_offset(ObjectHandle object) const;
    [[nodiscard]] std::uint32_t path_strategy_address() const noexcept {
        return path_strategy_address_;
    }
    // Every event since construction; nothing clears the list.
    [[nodiscard]] const std::vector<PathEvent>& events() const noexcept { return events_; }
    [[nodiscard]] const std::vector<std::uint8_t>& unsupported_opcodes() const noexcept {
        return unsupported_opcodes_;
    }

private:
    struct RuntimeState {
        struct Trigger {
            std::uint16_t path{};
            std::uint8_t condition{};
        };
        std::vector<std::uint16_t> stack;
        std::vector<Trigger> triggers;
        bool if_not{};
    };

    [[nodiscard]] std::uint8_t read8(std::uint16_t offset) const;
    [[nodiscard]] std::uint16_t read16(std::uint16_t offset) const;
    [[nodiscard]] std::int16_t read_i16(std::uint16_t offset) const;
    void set_path_offset(ObjectHandle object, std::uint16_t offset);
    void move(ObjectHandle object);
    void generate_vectors(GameObject& object) const noexcept;
    [[nodiscard]] ObjectHandle spawn(ObjectHandle parent, std::uint16_t shape);
    [[nodiscard]] std::uint8_t chase_byte(std::uint8_t current, std::uint8_t target) const noexcept;
    [[nodiscard]] std::uint16_t chase_word(std::uint16_t current, std::uint16_t target) const noexcept;

    const assets::RomImage* rom_{};
    std::uint32_t paths_address_{};
    std::uint32_t path_strategy_address_{};
    std::uint32_t particle_strategy_address_{};
    std::uint16_t null_shape_{};
    ObjectPool* objects_{};
    TrigTables trig_;
    OriginalPrng* random_{};
    ObjectHandle player_{};
    ObjectHandle pending_link_{};
    std::int16_t view_velocity_z_{};
    std::uint8_t current_level_{};
    bool player_dead_{};
    std::uint16_t player_score_{};
    std::uint16_t player_score_address_{};
    std::array<std::uint8_t, 256> opcode_translation_{};
    // P_IMPORT/P_EXPORT address the shared 64 KiB direct-page namespace in
    // bank $7e. The native strategy scheduler uses Wdc65816's real WRAM;
    // this model keeps its own copy of that bank for host tests.
    std::array<std::uint8_t, 65'536> global_memory_{};
    std::unordered_map<ObjectHandle, RuntimeState> states_;
    std::vector<PathEvent> events_;
    std::vector<std::uint8_t> unsupported_opcodes_;
};

} // namespace starfox::simulation
