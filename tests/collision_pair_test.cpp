#include "starfox/assets/rom.hpp"
#include "starfox/simulation/map_vm.hpp"
#include "starfox/simulation/object_pool.hpp"
#include "starfox/simulation/strategy_scheduler.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using starfox::assets::RomImage;
using starfox::assets::SymbolMap;
using starfox::simulation::MapDatabase;
using starfox::simulation::MapVm;
using starfox::simulation::ObjectMemoryLayout;
using starfox::simulation::ObjectPool;
using starfox::simulation::Wdc65816Registers;

void check(bool condition, const std::string& context) {
    if (!condition) throw std::runtime_error(context);
}

void compare_registers(const Wdc65816Registers& a,
                       const Wdc65816Registers& b,
                       const std::string& context) {
    check(a.a == b.a, context + " A");
    check(a.x == b.x, context + " X");
    check(a.y == b.y, context + " Y");
    check(a.direct == b.direct, context + " D");
    check(a.stack == b.stack, context + " SP");
    check(a.data_bank == b.data_bank, context + " DB");
    check(a.status == b.status, context + " P");
}

Wdc65816Registers entry_registers() {
    Wdc65816Registers registers{};
    registers.status = 0x24U;
    return registers;
}

void check_pair(const RomImage& rom, const SymbolMap& symbols,
                const ObjectPool& fixture_objects, const MapVm& fixture_map,
                const std::string& context, unsigned& nonempty_lists) {
    const auto builder = symbols.find("GENERATE_COLLIST_L").at(0);
    const auto resolver = symbols.find("INIT_STRATS_RAM_L").at(0);
    const auto count_address = symbols.find("COLLISTCNT").at(0);
    const auto capacity = static_cast<std::size_t>(symbols.find("NUMBER_AL").at(0));
    const auto layout = symbols.find("NUMBER_AL").at(0) == 0x50U
        ? ObjectMemoryLayout::starfox_ex : ObjectMemoryLayout::original;
    ObjectPool reference_objects{capacity, layout};
    ObjectPool deferred_objects{capacity, layout};
    reference_objects.load_state(fixture_objects.save_state());
    deferred_objects.load_state(fixture_objects.save_state());
    MapVm reference{rom, MapDatabase{rom, symbols}, reference_objects, &symbols};
    MapVm deferred{rom, MapDatabase{rom, symbols}, deferred_objects, &symbols};
    reference.load_state(fixture_map.save_state());
    deferred.load_state(fixture_map.save_state());

    auto reference_regs = entry_registers();
    auto deferred_regs = entry_registers();
    const auto reference_builder_count = reference.call_native_routine(
        builder, reference_regs, 5'000'000U, false, true, false);
    const auto deferred_builder_count = deferred.call_native_routine(
        builder, deferred_regs, 5'000'000U, false, false, false);
    check(reference_builder_count == deferred_builder_count,
          context + " builder instruction count");
    compare_registers(reference_regs, deferred_regs, context + " builder");
    const auto reference_list_count = reference.read_native_byte(count_address);
    const auto deferred_list_count = deferred.read_native_byte(count_address);
    check(reference_list_count == deferred_list_count,
          context + " generated collision count");
    nonempty_lists += reference_list_count != 0U;

    reference_regs = entry_registers();
    deferred_regs = entry_registers();
    const auto reference_resolver_count = reference.call_native_routine(
        resolver, reference_regs, 10'000'000U, false, true, false);
    const auto deferred_resolver_count = deferred.call_native_routine(
        resolver, deferred_regs, 10'000'000U, false, true, false);
    check(reference_resolver_count == deferred_resolver_count,
          context + " resolver instruction count");
    compare_registers(reference_regs, deferred_regs, context + " resolver");
    check(reference_objects.save_state() == deferred_objects.save_state(),
          context + " final object pool");
    check(reference.save_state() == deferred.save_state(),
          context + " final map and emulated CPU state");
    std::cout << context << " objects=" << fixture_objects.active_count()
              << " collisions=" << static_cast<unsigned>(reference_list_count)
              << " instructions=" << reference_builder_count << '+'
              << reference_resolver_count << " PASS\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: collision_pair_test PATCHED_ROM SYMBOL_MAP\n";
        return 2;
    }
    try {
        const auto rom = RomImage::load(argv[1]);
        const auto symbols = SymbolMap::load(argv[2]);
        const auto layout = symbols.find("NUMBER_AL").at(0) == 0x50U
            ? ObjectMemoryLayout::starfox_ex : ObjectMemoryLayout::original;
        ObjectPool objects{static_cast<std::size_t>(
            symbols.find("NUMBER_AL").at(0)), layout};
        const auto player = objects.allocate_after();
        MapVm map{rom, MapDatabase{rom, symbols}, objects, &symbols};
        auto boot_regs = entry_registers();
        map.call_native_routine(symbols.find("COPY_TO_0101_L").at(0),
                                boot_regs, 5'000'000U);
        map.start(symbols.find("MAP1_1A").at(0), player);
        map.advance_distance(1);
        check(objects.active_count() > 1U, "map fixture has no spawned objects");
        starfox::simulation::NativeStrategyScheduler strategies{symbols, objects, map};
        const auto stats = strategies.tick_all();
        check(stats.objects_run > 1U && stats.instructions > 0U,
              "map fixture did not run native strategies");

        unsigned nonempty_lists = 0;
        // Each arrangement starts from the real Corneria post-strategy state.
        // A common native call pushes host movement into WRAM before cloning;
        // the measured pair itself uses the production no-push arguments.
        for (int arrangement = 0; arrangement < 16; ++arrangement) {
            ObjectPool fixture_objects{objects.capacity(), layout};
            fixture_objects.load_state(objects.save_state());
            MapVm fixture_map{rom, MapDatabase{rom, symbols}, fixture_objects, &symbols};
            fixture_map.load_state(map.save_state());
            if (arrangement != 0) {
                if (arrangement >= 3) {
                    auto template_object = fixture_objects.at(player);
                    for (const auto handle : fixture_objects.active_handles())
                        if (fixture_objects.at(handle).shape != 0U) {
                            template_object = fixture_objects.at(handle);
                            break;
                        }
                    check(template_object.shape != 0U, "missing real shaped template");
                    // Dense and sparse arrangements extend the real map's
                    // objects without running uninitialized strategies.
                    const auto target = std::min<std::size_t>(fixture_objects.capacity(),
                        static_cast<std::size_t>(arrangement * 6));
                    while (fixture_objects.active_count() < target) {
                        const auto handle = fixture_objects.allocate_after();
                        check(handle != 0U, "collision fixture allocation failed");
                        fixture_objects.at(handle) = template_object;
                        auto& added = fixture_objects.at(handle);
                        const auto spacing = arrangement % 2 ? 12 : 300;
                        added.world_x = static_cast<std::int16_t>((int(handle) % 8 - 4) * spacing);
                        added.world_y = static_cast<std::int16_t>((int(handle) / 8 - 4) * spacing);
                        added.world_z = static_cast<std::int16_t>(600 + int(handle) * spacing);
                    }
                }
                auto& moving = fixture_objects.at(player);
                moving.world_x = static_cast<std::int16_t>(
                    moving.world_x + arrangement * 96);
                moving.world_y = static_cast<std::int16_t>(
                    moving.world_y - arrangement * 48);
                unsigned enabled = 0;
                for (const auto handle : fixture_objects.active_handles()) {
                    auto& object = fixture_objects.at(handle);
                    if (object.shape == 0U) continue;
                    object.health = 10U;
                    object.flags &= static_cast<std::uint8_t>(~1U); // AFEXP
                    object.collision_flags &= static_cast<std::uint8_t>(~4U);
                    object.strategy_flags[0] &= static_cast<std::uint8_t>(~1U);
                    ++enabled;
                    if (arrangement < 3 && enabled == 2U) break;
                }
                check(enabled >= 2U, "fixture has fewer than two shaped objects");
                auto setup_regs = entry_registers();
                fixture_map.call_native_routine(
                    symbols.find("GENERATE_COLLIST_L").at(0),
                    setup_regs, 5'000'000U);
            }
            check_pair(rom, symbols, fixture_objects, fixture_map,
                       "arrangement " + std::to_string(arrangement),
                       nonempty_lists);
        }
        check(nonempty_lists != 0U, "all real-ROM collision lists were empty");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
