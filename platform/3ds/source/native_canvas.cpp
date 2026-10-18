#include "native_canvas.hpp"
#include "starfield_3ds.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

namespace starfox::platform_3ds {

void compose_native_mode1(const render::Framebuffer& source,
    const render::Framebuffer& world, const render::Framebuffer& bg2_low,
    const render::Framebuffer& bg2_high, const render::Framebuffer& bg3_low,
    const render::Framebuffer& bg3_high, render::Framebuffer& target,
    bool bg3_high_priority) noexcept {
    if (source.width() != 256U || source.height() != 224U
        || source.draw_scale() != 1U) return;
    const auto native = [](const render::Framebuffer& frame) {
        return frame.width() == 400U && frame.height() == 240U
            && frame.draw_scale() == 1U;
    };
    if (!native(world) || !native(bg2_low) || !native(bg2_high)
        || !native(bg3_low) || !native(bg3_high) || !native(target)) return;
    auto& pixels = target.pixels();
    if (target.layer_tags_enabled() || target.tracks_write_coverage()
        || target.command_buffer() != nullptr) {
        target.clear();
        render::composite_transparent_layer(bg3_low, target, {});
        if (!bg3_high_priority)
            render::composite_transparent_layer(bg3_high, target, {});
        render::composite_transparent_layer(bg2_low, target, {});
        render::composite_transparent_layer(world, target, {});
        render::composite_transparent_layer(bg2_high, target, {});
        if (bg3_high_priority)
            render::composite_transparent_layer(bg3_high, target, {});
    } else {
        const auto* front = bg2_high.pixels().data();
        const auto* models = world.pixels().data();
        const auto* back = bg2_low.pixels().data();
        const auto* panorama = bg3_low.pixels().data();
        const auto* panorama_high = bg3_high.pixels().data();
        const auto margin = [&](std::size_t first, std::size_t last) {
            for (auto i = first; i < last; ++i) {
                auto colour = bg3_high_priority ? panorama_high[i] : 0U;
                if (colour == 0U) colour = front[i];
                if (colour == 0U) colour = models[i];
                if (colour == 0U) colour = back[i];
                if (colour == 0U && !bg3_high_priority) colour = panorama_high[i];
                if (colour == 0U) colour = panorama[i];
                pixels[i] = static_cast<std::uint8_t>(colour);
            }
        };
        margin(0U, 8U * 400U);
        for (std::size_t y = 8U; y < 232U; ++y) {
            margin(y * 400U, y * 400U + 72U);
            margin(y * 400U + 328U, (y + 1U) * 400U);
        }
        margin(232U * 400U, pixels.size());
    }
    for (std::size_t y = 0U; y < 224U; ++y)
        std::copy_n(source.pixels().data() + y * 256U, 256U,
            pixels.data() + (y + 8U) * 400U + 72U);
}

namespace {

std::uint8_t dominant_edge(const render::Framebuffer& source,
    std::uint32_t x, std::uint32_t first_y = 0U,
    std::uint32_t last_y = 224U) noexcept {
    std::array<std::uint16_t, 256U> counts{};
    const auto& pixels = source.pixels();
    for (auto y = first_y; y < last_y; ++y)
        ++counts[pixels[static_cast<std::size_t>(y) * 256U + x]];
    return static_cast<std::uint8_t>(std::distance(counts.begin(),
        std::max_element(counts.begin(), counts.end())));
}

std::uint8_t dominant_band(std::span<const std::uint8_t> colours,
    std::uint32_t first_y) noexcept {
    std::array<std::uint8_t, 256U> counts{};
    for (auto y = first_y;
        y < std::min<std::uint32_t>(first_y + 8U, 224U); ++y)
        ++counts[colours[y]];
    return static_cast<std::uint8_t>(std::distance(counts.begin(),
        std::max_element(counts.begin(), counts.end())));
}

std::uint8_t nearest_star_colour(const simulation::SnesPpuState& ppu,
    std::uint32_t target_red, std::uint32_t target_green,
    std::uint32_t target_blue) noexcept {
    std::uint8_t best = 0U;
    std::uint32_t best_distance = 251U;
    for (std::uint32_t index = 1U; index < ppu.cgram.size(); ++index) {
        const auto colour = ppu.cgram[index];
        const auto red = colour & 31U;
        const auto green = (colour >> 5U) & 31U;
        const auto blue = (colour >> 10U) & 31U;
        // The map's authored stars use muted sage-gray arms and a lighter
        // centre. Exclude the white menu font and the dark slate backdrop.
        if (std::min({red, green, blue}) < 9U
            || std::max({red, green, blue}) > 27U) continue;
        const auto delta_red = static_cast<std::int32_t>(red)
            - static_cast<std::int32_t>(target_red);
        const auto delta_green = static_cast<std::int32_t>(green)
            - static_cast<std::int32_t>(target_green);
        const auto delta_blue = static_cast<std::int32_t>(blue)
            - static_cast<std::int32_t>(target_blue);
        const auto distance = static_cast<std::uint32_t>(
            delta_red * delta_red + delta_green * delta_green
                + delta_blue * delta_blue);
        if (distance < best_distance) {
            best_distance = distance;
            best = static_cast<std::uint8_t>(index);
        }
    }
    return best;
}

std::uint8_t nearest_colour(const simulation::SnesPpuState& ppu,
    int red,int green,int blue) noexcept {
    std::uint8_t best=0U;
    unsigned best_distance=~0U;
    for(unsigned i=1;i<ppu.cgram.size();++i) {
        const auto c=ppu.cgram[i];
        const int r=int(c&31U),g=int((c>>5U)&31U),b=int((c>>10U)&31U);
        if(std::max({r,g,b})<8) continue;
        const unsigned distance=unsigned((r-red)*(r-red)
            +(g-green)*(g-green)+(b-blue)*(b-blue));
        if(distance<best_distance) {
            best_distance=distance;best=static_cast<std::uint8_t>(i);
        }
    }
    return best;
}

struct StarPoint {
    std::uint16_t x;
    std::uint8_t y;
    bool cross;
};

// Hand-placed so there are no adjoining dots or accidentally elongated
// clusters. The four crosses on the map follow the source sprite's exact
// five-pixel silhouette; the remaining stars are individual pixels.
constexpr std::array<StarPoint, 22U> map_stars{{
    {12, 19, false}, {49, 30, false}, {25, 53, true},
    {59, 72, false}, {9, 96, false}, {45, 112, false},
    {18, 139, true}, {57, 157, false}, {34, 180, false},
    {7, 209, false}, {56, 222, false},
    {345, 15, false}, {384, 37, false}, {365, 55, true},
    {338, 83, false}, {390, 102, false}, {355, 120, false},
    {379, 145, true}, {341, 170, false}, {392, 185, false},
    {361, 211, false}, {387, 227, false},
}};
constexpr std::array<StarPoint, 8U> controls_stars{{
    {13, 30, false}, {28, 92, true}, {8, 170, false},
    {24, 215, false}, {373, 25, false}, {389, 86, true},
    {367, 160, false}, {382, 220, false},
}};
constexpr std::array<StarPoint, 14U> space_stars{{
    {10, 12, false}, {52, 37, false}, {27, 68, true},
    {61, 101, false}, {14, 151, false}, {46, 186, false},
    {23, 224, false}, {345, 22, false}, {378, 48, false},
    {352, 83, true}, {390, 122, false}, {339, 159, false},
    {374, 194, false}, {395, 225, false},
}};

// Precompute only the native side columns once; the source 256-pixel sky
// remains untouched. This gives both columns the measured cartridge density.
const std::vector<StarPoint>& game_over_stars() {
    static const auto stars = [] {
        std::vector<StarPoint> result;
        for (unsigned y=0;y<240U;++y)
            for (unsigned x=0;x<400U;++x)
                if ((x<72U || x>=328U)
                    && (game_over_star_hash(x,y)&4095U)<11U)
                    result.push_back({static_cast<std::uint16_t>(x),
                        static_cast<std::uint8_t>(y),false});
        return result;
    }();
    return stars;
}

bool is_isolated_map_artifact(const render::Framebuffer& source,
    std::uint8_t backdrop, const simulation::SnesPpuState& ppu) noexcept {
    // Planet selection sometimes leaves one wrapped eight-pixel OBJ scanline
    // at the original raster's top-left corner. Only remove that exact,
    // isolated neon-green row; legitimate planet art remains untouched.
    const auto& pixels = source.pixels();
    const auto stray = pixels[7U * 256U];
    if (stray == backdrop || pixels[7U * 256U + 8U] != backdrop)
        return false;
    const auto colour = ppu.cgram[stray];
    const auto red = colour & 31U;
    const auto green = (colour >> 5U) & 31U;
    const auto blue = (colour >> 10U) & 31U;
    if (green < 20U || green < red * 2U || green < blue * 2U)
        return false;
    for (std::uint32_t x = 0U; x < 8U; ++x) {
        if (pixels[7U * 256U + x] != stray
            || pixels[6U * 256U + x] != backdrop
            || pixels[8U * 256U + x] != backdrop) return false;
    }
    return true;
}

} // namespace

void build_native_panorama_atlas(const render::Framebuffer& source_low,
    const render::Framebuffer& source_high, render::Framebuffer& target_low,
    render::Framebuffer& target_high,
    const simulation::SnesPpuState& ppu) {
    const auto width = source_low.width();
    const auto height = source_low.height();
    if (width < 256U || width > 1024U || height < 256U || height > 1024U
        || (width & (width - 1U)) != 0U || (height & (height - 1U)) != 0U
        || source_high.width() != width || source_high.height() != height) return;
    const auto atlas_width = width + native_panorama_padding * 2U;
    target_low.resize(atlas_width, height);
    target_high.resize(atlas_width, height);
    target_low.clear(); target_high.clear();
    const auto& low = source_low.pixels();
    const auto& high = source_high.pixels();
    auto& wide_low = target_low.pixels();
    auto& wide_high = target_high.pixels();
    struct Colour { int r, g, b; };
    std::array<Colour, 256U> colours{};
    for (unsigned i = 0U; i < colours.size(); ++i) {
        const auto c = ppu.cgram[i];
        colours[i] = {int(c & 31U), int((c >> 5U) & 31U), int((c >> 10U) & 31U)};
    }
    const auto colour_distance = [&colours](std::uint8_t a, std::uint8_t b) {
        const auto& x = colours[a]; const auto& y = colours[b];
        return unsigned(std::abs(x.r - y.r) + std::abs(x.g - y.g) + std::abs(x.b - y.b));
    };
    // Mode 1 BG3 uses eight four-colour palettes. Seam matching repeatedly
    // compares the same colours, so retain their exact RGB distances.
    std::array<std::array<std::uint8_t, 32U>, 32U> distances{};
    for (unsigned a = 0U; a < distances.size(); ++a)
        for (unsigned b = 0U; b < distances[a].size(); ++b)
            distances[a][b] = static_cast<std::uint8_t>(colour_distance(a, b));
    const auto distance = [&distances, &colour_distance](std::uint8_t a, std::uint8_t b) {
        return a < 32U && b < 32U ? unsigned(distances[a][b])
            : colour_distance(a, b);
    };
    const auto source_colour = [&low, &high, width](unsigned x, unsigned y) {
        const auto i = y * width + x;
        return high[i] != 0U ? high[i] : low[i];
    };
    const auto native_colour = [&wide_low, &wide_high, atlas_width](unsigned x, unsigned y) {
        const auto i = y * atlas_width + x;
        return wide_high[i] != 0U ? wide_high[i] : wide_low[i];
    };
    for (unsigned y = 0U; y < height; ++y) {
        std::copy_n(low.data() + y * width, width,
            wide_low.data() + y * atlas_width + native_panorama_padding);
        std::copy_n(high.data() + y * width, width,
            wide_high.data() + y * atlas_width + native_panorama_padding);
    }
    const auto patches = width / 8U;
    std::vector<unsigned> density(patches), uses(patches);
    std::vector<bool> allowed(patches);
    for (unsigned band = 0U; band < height / 8U; ++band) {
        const auto first_y = band * 8U;
        std::array<unsigned, 256U> histogram{};
        for (unsigned y = first_y; y < first_y + 8U; ++y)
            for (unsigned x = 0U; x < width; ++x) ++histogram[source_colour(x, y)];
        const auto dominant = static_cast<std::uint8_t>(std::distance(
            histogram.begin(), std::max_element(histogram.begin(), histogram.end())));
        std::fill(density.begin(), density.end(), 0U);
        std::fill(uses.begin(), uses.end(), 0U);
        unsigned density_sum{}, allowed_count{};
        for (unsigned patch = 0U; patch < patches; ++patch) {
            bool feature = false;
            for (unsigned dy = 0U; dy < 8U; ++dy) {
                for (unsigned dx = 0U; dx < 8U; ++dx) {
                    const auto c = source_colour(patch * 8U + dx, first_y + dy);
                    density[patch] += distance(c, dominant) != 0U;
                    const auto rgb = colours[c];
                    // The distant bright moon/eye is unique art. Texture
                    // donors retain its surrounding dark terrain, while the
                    // lower horizon and star field retain their bright ink.
                    if (first_y < 112U && (rgb.r + rgb.g + rgb.b >= 58
                            || (rgb.r > 17 && rgb.g > 12 && rgb.b <= 12))) feature = true;
                }
            }
            allowed[patch] = !feature;
            if (!feature) { density_sum += density[patch]; ++allowed_count; }
        }
        const auto target_density = allowed_count != 0U ? density_sum / allowed_count : 0U;
        for (unsigned side = 0U; side < 2U; ++side) {
            for (unsigned step = 0U; step < native_panorama_padding / 8U; ++step) {
                const auto start_x = side == 0U
                    ? native_panorama_padding - 8U - step * 8U
                    : native_panorama_padding + width + step * 8U;
                const auto neighbour_x = side == 0U ? start_x + 8U : start_x - 1U;
                auto best_score = ~0U;
                unsigned best = patches;
                // Rotate equal-score selection deterministically; adjacent
                // bands and opposite margins do not stamp the same pattern.
                const auto phase = (band * 13U + side * 11U + step * 7U) % patches;
                for (unsigned candidate = 0U; candidate < patches; ++candidate) {
                    const auto patch = (candidate + phase) % patches;
                    if (!allowed[patch]) continue;
                    unsigned score = unsigned(std::abs(int(density[patch]) - int(target_density)))
                        + uses[patch] * 4U;
                    if (score >= best_score) continue;
                    const auto source_x = patch * 8U;
                    // Every term is nonnegative. A donor that has already
                    // lost cannot become the winner; equal scores retain
                    // the original first-candidate tie break.
                    for (unsigned dy = 0U; dy < 8U && score < best_score; ++dy)
                        score += 4U * distance(source_colour(source_x + (side == 0U ? 7U : 0U), first_y + dy),
                            native_colour(neighbour_x, first_y + dy));
                    if (score >= best_score) continue;
                    if (band != 0U) {
                        for (unsigned dx = 0U; dx < 8U && score < best_score; ++dx)
                            score += distance(source_colour(source_x + dx, first_y),
                                native_colour(start_x + dx, first_y - 1U));
                    }
                    if (score < best_score) {
                        best_score = score; best = patch;
                        if (best_score == 0U) break;
                    }
                }
                if (best != patches) ++uses[best];
                for (unsigned dy = 0U; dy < 8U; ++dy) {
                    for (unsigned dx = 0U; dx < 8U; ++dx) {
                        const auto dst = (first_y + dy) * atlas_width + start_x + dx;
                        if (best != patches) {
                            const auto src = (first_y + dy) * width + best * 8U + dx;
                            wide_low[dst] = low[src]; wide_high[dst] = high[src];
                        } else {
                            // No eligible donor: continue the local material,
                            // retaining the priority of its neighbouring texel.
                            const auto src = (first_y + dy) * atlas_width + neighbour_x;
                            wide_low[dst] = wide_low[src]; wide_high[dst] = wide_high[src];
                        }
                    }
                }
            }
        }
    }
}

void scroll_native_panorama_atlas(const render::Framebuffer& atlas_low,
    const render::Framebuffer& atlas_high, render::Framebuffer& target_low,
    render::Framebuffer& target_high,
    const simulation::SnesPpuState& ppu) noexcept {
    if (atlas_low.width() < native_panorama_padding * 2U + 256U
        || atlas_low.width() != atlas_high.width()
        || atlas_low.height() < 256U || atlas_low.height() != atlas_high.height()
        || (atlas_low.height() & (atlas_low.height() - 1U)) != 0U
        || target_low.width() != 400U || target_low.height() != 240U
        || target_high.width() != 400U || target_high.height() != 240U) return;
    // The simulation retains signed HOFS; handle equivalent ten-bit register
    // representations without wrapping a second copy of the entire picture.
    const auto scroll_x = static_cast<int>((static_cast<unsigned>(
        ppu.bg3_scroll_x) + 512U) & 1023U) - 512;
    const auto width = atlas_low.width();
    const auto height_mask = atlas_low.height() - 1U;
    const auto mosaic = (ppu.mosaic & 0x04U) != 0U
        ? static_cast<int>((ppu.mosaic >> 4U) + 1U) : 1;
    const auto snap = [mosaic](int coordinate) {
        const int remainder = coordinate % mosaic;
        return coordinate - (remainder < 0 ? remainder + mosaic : remainder);
    };
    for (unsigned y = 0U; y < 240U; ++y) {
        const auto logical_y = std::clamp(static_cast<int>(y) - 8, 0, 223);
        const auto source_y = static_cast<unsigned>(snap(logical_y)
            + ppu.bg3_scroll_y) & height_mask;
        for (auto pair : {std::pair{&atlas_low, &target_low},
                          std::pair{&atlas_high, &target_high}}) {
            const auto* source = pair.first->pixels().data() + source_y * width;
            auto* destination = pair.second->pixels().data() + y * 400U;
            if (mosaic == 1) {
                const auto first = static_cast<int>(native_panorama_padding) - 72 + scroll_x;
                std::copy_n(source + first, 400U, destination);
            } else {
                for (unsigned x = 0U; x < 400U; ++x)
                    destination[x] = source[native_panorama_padding
                        + snap(static_cast<int>(x) - 72) + scroll_x];
            }
        }
    }
}

void compose_native_canvas(const render::Framebuffer& source,
    const render::Framebuffer& world, render::Framebuffer& target,
    const simulation::SnesPpuState& ppu, NativeCanvasStyle style,
    std::span<const std::uint8_t> background_left,
    std::span<const std::uint8_t> background_right,
    int intro_vertical_shift) noexcept {
    if (source.width() != 256U || source.height() != 224U
        || world.width() != 400U || world.height() != 240U
        || target.width() != 400U || target.height() != 240U) return;
    const auto& source_pixels = source.pixels();
    const auto& world_pixels = world.pixels();
    auto& pixels = target.pixels();
    constexpr auto left = 72U;
    constexpr auto right = 328U;
    const auto fill_left = dominant_edge(source, 0U);
    const auto fill_right = dominant_edge(source, 255U);
    const auto spread_controls = style == NativeCanvasStyle::controls
        && fill_left == fill_right
        && [&] {
            for (std::uint32_t y = 0U; y < 224U; ++y) {
                for (std::uint32_t x = 0U; x < 16U; ++x) {
                    if (source_pixels[static_cast<std::size_t>(y) * 256U + x]
                        != fill_left) return false;
                }
            }
            return true;
        }();
    const bool continue_background_edges = style == NativeCanvasStyle::gameplay
        || style == NativeCanvasStyle::intro;
    std::array<std::uint8_t, 28U> left_bands{}, right_bands{};
    if (continue_background_edges) {
        // Every raster in an eight-line band uses the same source samples.
        // Compute its histogram once, retaining the original lowest-index
        // tie break and the separate background-only edges when available.
        for (std::uint32_t band = 0U; band < left_bands.size(); ++band) {
            const auto first = band * 8U;
            left_bands[band] = background_left.size() == 224U
                ? dominant_band(background_left, first)
                : dominant_edge(source, 0U, first, first + 8U);
            right_bands[band] = background_right.size() == 224U
                ? dominant_band(background_right, first)
                : dominant_edge(source, 255U, first, first + 8U);
        }
    }
    for (std::uint32_t y = 0U; y < 240U; ++y) {
        auto* row = pixels.data() + static_cast<std::size_t>(y) * 400U;
        auto left_colour = fill_left;
        auto right_colour = fill_right;
        if (continue_background_edges) {
            // Mode 1 can have a moving sky or floor. Continue its edge colour
            // in eight-line bands; an isolated star or model pixel must not
            // become a 72-pixel horizontal streak.
            const auto source_y = std::clamp<std::int32_t>(
                static_cast<std::int32_t>(y) - 8, 0, 223);
            const auto band = static_cast<std::uint32_t>(source_y) >> 3U;
            left_colour = left_bands[band];
            right_colour = right_bands[band];
        }
        std::fill_n(row, left, left_colour);
        std::fill_n(row + right, 400U - right, right_colour);
        if (y >= 8U && y < 232U) {
            const auto* source_row = source_pixels.data()
                + static_cast<std::size_t>(y - 8U) * 256U;
            if (spread_controls) {
                // The controls screen has two independent source panels and
                // a solid slate field. Move those panels as intact pixel
                // blocks into the unused native columns; no artwork scales.
                std::fill_n(row + left, 256U, fill_left);
                std::copy_n(source_row + 16U, 136U, row + 48U);
                std::copy_n(source_row + 152U, 104U, row + 256U);
            } else {
                std::copy_n(source_row, 256U, row + left);
                if (style == NativeCanvasStyle::star_map && y == 15U
                    && is_isolated_map_artifact(source, fill_left, ppu))
                    std::fill_n(row + left, 8U, fill_left);
            }
        } else {
            std::fill_n(row + left, 256U,
                (style == NativeCanvasStyle::gameplay
                    || style == NativeCanvasStyle::intro)
                    ? left_colour : y < 8U ? fill_left : fill_right);
        }
        if (style == NativeCanvasStyle::gameplay
            || style == NativeCanvasStyle::intro) {
            const auto* world_row = world_pixels.data()
                + static_cast<std::size_t>(y) * 400U;
            for (std::uint32_t x = 0U; x < 400U; ++x) {
                if (y >= 8U && y < 232U && x >= left && x < right)
                    continue;
                if (world_row[x] != 0U) row[x] = world_row[x];
            }
        }
    }
    if (style == NativeCanvasStyle::intro
        && !std::all_of(source_pixels.begin(), source_pixels.end(),
            [](std::uint8_t pixel) { return pixel == 0U; })) {
        // Sparse stars fill any black margin left empty by the live 3D dust.
        // Their vertical phase follows the camera's projected horizon, so
        // they rise with the centre during the planet approach.
        struct IntroStar {std::uint16_t x,y;std::uint8_t shade;};
        static const auto margin_stars=[] {
            std::vector<IntroStar> result;
            for(unsigned y=8U;y<232U;++y) for(unsigned x=0U;x<400U;++x) {
                if(x>=72U&&x<328U) {x=327U;continue;}
                const auto shade=game_over_star_hash(x,y,0x3c6ef372U);
                if((shade&4095U)<7U)
                    result.push_back({static_cast<std::uint16_t>(x),
                        static_cast<std::uint16_t>(y),
                        static_cast<std::uint8_t>((shade>>12U)%37U)});
            }
            return result;
        }();
        const std::array<std::uint8_t,4> colors{{
            nearest_colour(ppu,27,28,28), nearest_colour(ppu,15,19,27),
            nearest_colour(ppu,28,19,8), nearest_colour(ppu,25,9,8)}};
        for(const auto& star:margin_stars) {
            const auto shifted=int(star.y)-8+intro_vertical_shift;
            const auto y=8+(shifted%224+224)%224;
            const auto offset=std::size_t(y)*400U+star.x;
            if(world_pixels[offset]!=0U) continue;
            const auto existing=pixels[offset];
            const auto colour=ppu.cgram[existing];
            if(std::max({colour&31U,(colour>>5U)&31U,
                    (colour>>10U)&31U})>4U) continue;
            pixels[offset]=colors[star.shade==0U?3U:star.shade<3U?2U:
                star.shade<9U?1U:0U];
        }
    }
    std::span<const StarPoint> stars{};
    if (style == NativeCanvasStyle::star_map) stars = map_stars;
    else if (style == NativeCanvasStyle::controls) stars = controls_stars;
    else if (style == NativeCanvasStyle::space) stars = space_stars;
    else if (style == NativeCanvasStyle::game_over) stars = game_over_stars();
    if (stars.empty()
        || std::all_of(source_pixels.begin(), source_pixels.end(),
            [](std::uint8_t pixel) { return pixel == 0U; })) return;
    if(style==NativeCanvasStyle::game_over) {
        const std::array<std::uint8_t,4> colors{{
            nearest_colour(ppu,12,12,12),nearest_colour(ppu,24,24,24),
            nearest_colour(ppu,28,11,10),nearest_colour(ppu,29,22,9)}};
        for(std::size_t i=0;i<stars.size();++i) {
            const auto& point=stars[i];
            pixels[std::size_t(point.y)*400U+point.x]
                =colors[i%13U==0U?2U:i%17U==0U?3U:i%3U==0U?1U:0U];
        }
        return;
    }
    const auto arm = nearest_star_colour(ppu, 16U, 18U, 16U);
    const auto centre = nearest_star_colour(ppu, 23U, 25U, 23U);
    if (arm == 0U) return;
    for (std::size_t i=0;i<stars.size();++i) {
        const auto& point=stars[i];
        const auto x = static_cast<std::size_t>(point.x);
        const auto y = static_cast<std::size_t>(point.y);
        const auto offset = y * 400U + x;
        if (style == NativeCanvasStyle::game_over) {
            const auto shade = game_over_star_hash(point.x,point.y) >> 12U;
            pixels[offset] = shade % 17U == 0U
                ? nearest_colour(ppu,10,20,29)
                : shade % 2U == 0U && centre != 0U ? centre : arm;
        } else pixels[offset] = point.cross && centre != 0U ? centre : arm;
        if (point.cross) {
            pixels[offset - 400U] = arm;
            pixels[offset + 400U] = arm;
            pixels[offset - 1U] = arm;
            pixels[offset + 1U] = arm;
        }
    }
}

void capture_native_coverage_spans(const render::Framebuffer& source,
    std::vector<NativeCoverageSpan>& spans) {
    spans.clear();
    if (source.width() != 256U || source.height() != 224U) return;
    const auto coverage = source.write_coverage();
    if (coverage.size() != 256U * 224U) return;
    for (std::uint32_t y = 0U; y < 224U; ++y) {
        const auto row = y * 256U;
        for (std::uint32_t x = 0U; x < 256U;) {
            while (x < 256U && coverage[row + x] == 0U) ++x;
            const auto first = x;
            while (x < 256U && coverage[row + x] != 0U) ++x;
            if (x != first) spans.push_back({row + first,
                (y + 8U) * 400U + 72U + first, x - first});
        }
    }
}

void overlay_source_coverage_on_native(const render::Framebuffer& source,
    render::Framebuffer& target,
    std::span<const NativeCoverageSpan> spans) noexcept {
    if (source.width() != 256U || source.height() != 224U
        || target.width() != 400U || target.height() != 240U) return;
    const auto& pixels = source.pixels();
    auto& destination = target.pixels();
    const auto& source_tags = source.layer_tags();
    auto& destination_tags = target.layer_tags();
    const bool tagged = target.layer_tags_enabled()
        && destination_tags.size() == destination.size();
    const bool source_tagged = source.layer_tags_enabled()
        && source_tags.size() == pixels.size();
    for (const auto& span : spans) {
        std::copy_n(pixels.data() + span.source_offset, span.count,
            destination.data() + span.target_offset);
        target.mark_written(span.target_offset, span.count);
        if (tagged) {
            if (source_tagged)
                std::copy_n(source_tags.data() + span.source_offset, span.count,
                    destination_tags.data() + span.target_offset);
            else std::fill_n(destination_tags.data() + span.target_offset,
                span.count, static_cast<std::uint8_t>(render::PixelLayer::three_d));
        }
    }
}

void overlay_source_coverage_on_native(const render::Framebuffer& source,
    render::Framebuffer& target) noexcept {
    if (source.width() != 256U || source.height() != 224U
        || target.width() != 400U || target.height() != 240U) return;
    const auto coverage = source.write_coverage();
    const auto& source_pixels = source.pixels();
    if (coverage.size() != source_pixels.size()) return;
    auto& destination = target.pixels();
    auto& destination_tags = target.layer_tags();
    const auto& source_tags = source.layer_tags();
    const bool tagged = target.layer_tags_enabled()
        && destination_tags.size() == destination.size();
    const bool source_tagged = source.layer_tags_enabled()
        && source_tags.size() == source_pixels.size();
    for (std::size_t y = 0U; y < 224U; ++y) {
        const auto source_row = y * 256U;
        const auto destination_row = (y + 8U) * 400U + 72U;
        for (std::size_t x = 0U; x < 256U;) {
            while (x < 256U && coverage[source_row + x] == 0U) ++x;
            const auto first = x;
            while (x < 256U && coverage[source_row + x] != 0U) ++x;
            if (first == x) continue;
            // Bulk spans avoid one bounds check and coverage fill per pixel.
            std::copy_n(source_pixels.data() + source_row + first,
                x - first, destination.data() + destination_row + first);
            target.mark_written(destination_row + first, x - first);
            if (tagged) {
                if (source_tagged)
                    std::copy_n(source_tags.data() + source_row + first,
                        x - first, destination_tags.data()
                            + destination_row + first);
                else std::fill_n(destination_tags.data()
                    + destination_row + first, x - first,
                    static_cast<std::uint8_t>(render::PixelLayer::three_d));
            }
        }
    }
}

} // namespace starfox::platform_3ds
