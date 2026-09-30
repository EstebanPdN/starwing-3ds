#pragma once
#include <cstdint>
namespace starfox::render {
// The 256x224 cartridge canvas represents a 4:3 display, not square pixels.
// Expanded host canvases already encode their chosen widescreen proportions.
constexpr std::uint32_t presentation_width(std::uint32_t width,
    std::uint32_t height) noexcept {
    return height && std::uint64_t(width)*224U==std::uint64_t(height)*256U
        ? static_cast<std::uint32_t>((std::uint64_t(height)*4U+1U)/3U) : width;
}
constexpr float presentation_to_raster_x(float x,std::uint32_t width,
    std::uint32_t height) noexcept {
    const auto display= presentation_width(width,height);
    return display ? x*static_cast<float>(width)/static_cast<float>(display) : x;
}
}
