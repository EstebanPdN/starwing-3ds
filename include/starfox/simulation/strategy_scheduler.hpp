#pragma once

#include "starfox/assets/rom.hpp"
#include "starfox/simulation/map_vm.hpp"
#include "starfox/simulation/object_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

// The per-tick object strategy loop of TRANS.ASM. GameSimulation runs it once
// per 20 Hz source update; the strategy and game-init trace tools and host
// tests drive it directly.

namespace starfox::simulation {

// objects_run counts DO_STRAT_L calls, objects_removed REMOVEDEADAL_L calls,
// instructions the 65C816 instructions both executed.
struct StrategyTickStats {
    std::size_t objects_run{};
    std::size_t objects_removed{};
    std::size_t instructions{};
};

// Runs the cartridge's strategy dispatcher (DO_STRAT_L) on the 65C816 core,
// against the WRAM image that MapVm keeps in step with the host ObjectPool.
// GameSimulation owns cadence, input, rendering and presentation. Holds
// pointers to `objects` and `native_state`, which must outlive it.
class NativeStrategyScheduler {
    friend class GameSimulation;
public:
    // Throws std::runtime_error when a strategy symbol is missing and
    // std::invalid_argument for a zero per-object instruction limit.
    NativeStrategyScheduler(
        const assets::SymbolMap& symbols,
        ObjectPool& objects,
        MapVm& native_state,
        std::size_t object_instruction_limit = 1'000'000U);

    // One pass over the active list: DO_STRAT_L for each object, at most once
    // per object generation, then REMOVEDEADAL_L for an object that set
    // ALDEAD. Throws std::runtime_error when a strategy call fails (a
    // path-controlled object whose 65C816 call fails is removed instead) or
    // the pass exceeds 4096 steps.
    // force_full_object_sync is retained for differential simulation checks.
    // Production leaves it false and pushes the object pool only before the
    // first strategy; each completed native call imports its full result.
    // batch_object_payloads lets MapVm defer payload imports between calls
    // when eight or more objects are active.
    [[nodiscard]] StrategyTickStats tick_all(bool force_full_object_sync = false,
        bool batch_object_payloads = true);
    // Star Fox EX's NOOBJMODE pass: objects outside `protected_objects` are
    // removed through REMOVEDEADAL_L without running their strategy.
    [[nodiscard]] StrategyTickStats tick_all_no_objects(
        std::span<const ObjectHandle> protected_objects);
    // DO_STRAT_L for one active object, after pushing the host pool to WRAM.
    // Returns the instructions executed; a failed call is handled as in
    // tick_all().
    [[nodiscard]] std::size_t tick_object(ObjectHandle object);
    // Advances GAMEFRAME and runs INIT_STRATS_L; returns its instructions.
    [[nodiscard]] std::size_t begin_tick();

private:
    [[nodiscard]] std::size_t tick_object(
        ObjectHandle object, bool sync_objects_before_call);

    ObjectPool* objects_{};
    MapVm* native_state_{};
    std::uint32_t do_strategy_{};
    std::uint32_t initialize_strategies_{};
    std::uint32_t remove_dead_{};
    std::uint32_t path_strategy_begin_{};
    std::uint32_t path_data_begin_{};
    std::uint32_t alien_dead_{};
    std::uint32_t game_frame_{};
    std::uint32_t escape_camera_strategy_{};
    std::uint32_t escape_anchor_{};
    std::uint32_t game_flags_2_{};
    std::size_t object_instruction_limit_{};
};

} // namespace starfox::simulation
