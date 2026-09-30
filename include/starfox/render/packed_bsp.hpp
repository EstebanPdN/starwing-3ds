#pragma once
#include "starfox/assets/shape.hpp"
#include <array>
#include <cstdint>
#include <vector>
namespace starfox::render {
struct PackedBspNode {
    std::array<std::uint32_t,4> links{},batch{};
};
static_assert(sizeof(PackedBspNode)==32);
// Immutable model-local data. Face IDs address this owned face array, including
// line/sprite faces; geometry emitters must preserve that common index space.
struct PackedBsp {
    std::vector<PackedBspNode> nodes;
    std::vector<assets::Face> faces;
    std::vector<std::uint32_t> face_ids;
    std::uint32_t root{UINT32_MAX},output_capacity{},work_limit{},maximum_depth{};
};
// Flattened source shape.faces order is used without BSP or during explosion.
// Throws if the source graph exceeds the GPU's depth/output/work bounds.
// Missing links/batches, duplicate addresses and leaf precedence match the CPU.
[[nodiscard]] PackedBsp pack_bsp(const assets::Shape&,bool explosion=false);
}
