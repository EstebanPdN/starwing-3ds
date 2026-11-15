#pragma once

#include "starfox/render/software_renderer.hpp"
#include <bit>

namespace starfox::render {

// All objects in one world pass share the scene's 128 immutable colour
// entries. Only their three depth thresholds can vary. Keep the cache local
// to that pass so scene changes and separate ROM instances need no invalidation.
class SourceDepthTables {
public:
    SourceDepthTables(const assets::RomImage& rom, std::uint32_t address,
        std::uint16_t thresholds, std::uint16_t colours)
        : rom_(rom), address_(address), thresholds_(thresholds), colours_(colours) {}

    void apply(std::uint8_t object_offset, RenderPose& pose) {
        const auto bank = address_ & 0xff0000U;
        const auto pointer = object_offset == 0U ? thresholds_
            : static_cast<std::uint16_t>(address_
                + static_cast<std::uint16_t>(object_offset - 1U) * 4U);
        // Preserve the general function's rejection and exceptional-address
        // behaviour, including its partial writes, for malformed inputs.
        if (bank >= 0x7e0000U || thresholds_ < 0x8000U || thresholds_ > 0xfffdU
            || colours_ < 0x8000U || colours_ > 0xff80U
            || pointer < 0x8000U || pointer > 0xfffdU) {
            apply_source_depth_tables(rom_, address_, thresholds_, colours_, object_offset, pose);
            return;
        }
        if (!prepared_) {
            apply_source_depth_tables(rom_, address_, thresholds_, colours_, 0U, base_);
            prepared_ = true;
        }
        pose.depth_colour_tables = base_.depth_colour_tables;
        pose.depth_thresholds = base_.depth_thresholds;
        pose.has_depth_colour_tables = base_.has_depth_colour_tables;
        if (object_offset != 0U) {
            for (std::size_t i = 0U; i < pose.depth_thresholds.size(); ++i) {
                const auto encoded = std::bit_cast<std::int8_t>(
                    rom_.read8(bank | static_cast<std::uint16_t>(pointer + i)));
                pose.depth_thresholds[i] = static_cast<std::int16_t>(
                    -static_cast<std::int16_t>(encoded) * 256);
            }
        }
    }

private:
    const assets::RomImage& rom_;
    std::uint32_t address_;
    std::uint16_t thresholds_, colours_;
    RenderPose base_{};
    bool prepared_{};
};

} // namespace starfox::render
