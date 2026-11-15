#include "starfox/render/stereo_projection.hpp"
#include "starfox/render/software_renderer.hpp"
#include "starfox/render/face_material.hpp"
#include "starfox/render/source_shading.hpp"
#include "starfox/render/source_line_runs.hpp"
#include "starfox/render/word_math.hpp"

#include "starfox/simulation/math.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <numeric>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace starfox::render {
namespace {

using Vec3 = software_renderer_detail::Vec3;
using ScreenPoint = software_renderer_detail::ScreenPoint;
using TexturePoint = software_renderer_detail::TexturePoint;
using RasterVertex = software_renderer_detail::RasterVertex;
using WordProjection = software_renderer_detail::WordProjection;
using RasterPoint = software_renderer_detail::RasterPoint;

// The word primitives of simulation/math.cpp, inlined (see word_math.hpp).
using word::add16;
using word::arithmetic_shift_right;
using word::exact_lround;
using word::multiply_q15;
using word::subtract16;
using word::wrap16;

constexpr double kTau = 6.283185307179586476925286766559;

template <typename Record>
const Record* find_bsp_record(const std::vector<Record>& records,
    const std::vector<std::pair<std::uint32_t, std::size_t>>& lookup,
    std::uint32_t address) {
    if (lookup.empty()) {
        const auto found = std::find_if(records.begin(), records.end(),
            [address](const auto& record) { return record.address == address; });
        return found == records.end() ? nullptr : &*found;
    }
    const auto found = std::lower_bound(lookup.begin(), lookup.end(), address,
        [](const auto& entry, std::uint32_t key) {
            return entry.first < key;
        });
    if (found == lookup.end() || found->first != address
        || found->second >= records.size()) return nullptr;
    return &records[found->second];
}

std::int16_t rounded_word(double value) {
    return wrap16(
        static_cast<std::int64_t>(exact_lround(value)));
}

std::int16_t midpoint_word(std::int16_t a, std::int16_t b) {
    const auto sum = add16(a, b);
    // Super FX DIV2 is ASR with its documented -1 -> 0 correction.
    if (sum == -1) return 0;
    return wrap16(
        arithmetic_shift_right(sum, 1));
}

// Where camera-space edge a-b crosses z = 0: MCLIP.MC's word bisection when
// word_exact, otherwise the linear intersection.
Vec3 near_intersection(Vec3 a, Vec3 b, bool word_exact) {
    if (!word_exact) {
        const auto denominator = a.z - b.z;
        const auto amount = denominator == 0.0 ? 0.0 : a.z / denominator;
        return {
            a.x + (b.x - a.x) * amount,
            a.y + (b.y - a.y) * amount,
            0.0,
        };
    }

    auto ax = rounded_word(a.x);
    auto ay = rounded_word(a.y);
    auto az = rounded_word(a.z);
    auto bx = rounded_word(b.x);
    auto by = rounded_word(b.y);
    auto bz = rounded_word(b.z);
    if (az < 0 && bz >= 0) {
        std::swap(ax, bx);
        std::swap(ay, by);
        std::swap(az, bz);
    }

    // MCLIP.MC repeatedly bisects the camera-space edge with 16-bit ADD and
    // DIV2 instructions. Its final point is the first midpoint whose z is 0.
    for (auto iteration = 0; iteration < 32; ++iteration) {
        const auto middle_z = midpoint_word(az, bz);
        const auto middle_x = midpoint_word(ax, bx);
        const auto middle_y = midpoint_word(ay, by);
        if (middle_z == 0) {
            return {static_cast<double>(middle_x),
                static_cast<double>(middle_y), 0.0};
        }
        if (middle_z < 0) {
            if (middle_z == bz && middle_x == bx && middle_y == by) break;
            bx = middle_x;
            by = middle_y;
            bz = middle_z;
        } else {
            if (middle_z == az && middle_x == ax && middle_y == ay) break;
            ax = middle_x;
            ay = middle_y;
            az = middle_z;
        }
    }

    // Retain a safe fallback for malformed host-authored preview geometry.
    if (az == 0) return {static_cast<double>(ax), static_cast<double>(ay), 0.0};
    if (bz == 0) return {static_cast<double>(bx), static_cast<double>(by), 0.0};
    const auto denominator = static_cast<double>(az) - bz;
    const auto amount = denominator == 0.0 ? 0.0
        : static_cast<double>(az) / denominator;
    return {
        std::trunc(ax + (bx - ax) * amount),
        std::trunc(ay + (by - ay) * amount),
        0.0,
    };
}

bool clip_near_line(Vec3& a, Vec3& b, bool word_exact) {
    const auto a_behind = a.z < 0.0;
    const auto b_behind = b.z < 0.0;
    if (a_behind && b_behind) return false;
    if (a_behind) a = near_intersection(a, b, word_exact);
    if (b_behind) b = near_intersection(a, b, word_exact);
    return true;
}

void clip_near_polygon(
    const std::vector<Vec3>& input, bool word_exact,
    std::vector<Vec3>& output) {
    output.clear();
    if (input.empty()) return;
    output.reserve(input.size() + 2U);
    for (std::size_t index = 0; index < input.size(); ++index) {
        const auto& current = input[index];
        const auto next_index = index + 1U == input.size()
            ? std::size_t{0U} : index + 1U;
        const auto& next = input[next_index];
        const auto current_inside = current.z >= 0.0;
        const auto next_inside = next.z >= 0.0;
        if (current_inside) {
            output.push_back(current);
            if (!next_inside) {
                output.push_back(near_intersection(current, next, word_exact));
            }
        } else if (next_inside) {
            output.push_back(near_intersection(current, next, word_exact));
        }
    }
}

// Projects a camera-space point onto the source raster around the vanishing
// point, with inward stereo disparity. word_exact at focal length 256 follows
// MDO_PROJECT's word arithmetic; `visible` means z >= 0.
ScreenPoint project_point(
    const Vec3& point,
    double focal_length,
    bool word_exact,
    double vanish_x,
    double vanish_y,
    bool subpixel = false, std::int32_t stereo_eye = 0) {
    // Undo the camera translation for the source-exact central projection,
    // then apply only inward disparity. Visibility/BSP retain the eye pose.
    const auto projection_x = point.x + stereo_eye;
    const auto stereo_shift = inward_stereo_shift(stereo_eye, point.z);
    if (word_exact && focal_length == 256.0) {
        const auto original_z = rounded_word(point.z);
        auto z = original_z;
        if (z < 0) z = wrap16(-static_cast<std::int32_t>(z));
        if (z == 0) z = 1;
        const auto magnitude = [](std::int16_t value) {
            return value < 0
                ? static_cast<std::uint16_t>(
                    -static_cast<std::uint16_t>(value))
                : static_cast<std::uint16_t>(value);
        };
        const auto x_word = rounded_word(projection_x);
        const auto y_word = rounded_word(point.y);
        const auto x_magnitude = magnitude(x_word);
        const auto y_magnitude = magnitude(y_word);
        const auto z_magnitude = static_cast<std::uint16_t>(z);
        std::uint16_t projected_x{};
        std::uint16_t projected_y{};
        const auto dominant = std::max(x_magnitude, y_magnitude);
        if (dominant != 0U) {
            // MDO_PROJECT performs an unsigned (coordinate*256)/z. Once the
            // dominant quotient reaches 16384 it saturates that axis at
            // 16383 and derives the minor axis from the original ratio.
            const auto dominant_quotient =
                (static_cast<std::uint32_t>(dominant) << 8U) / z_magnitude;
            if (dominant_quotient >= 16'384U) {
                const auto minor = std::min(x_magnitude, y_magnitude);
                const auto minor_projected = static_cast<std::uint16_t>(
                    static_cast<std::uint32_t>(minor) * 16'383U / dominant);
                if (x_magnitude >= y_magnitude) {
                    projected_x = 16'383U;
                    projected_y = minor_projected;
                } else {
                    projected_y = 16'383U;
                    projected_x = minor_projected;
                }
            } else {
                projected_x = static_cast<std::uint16_t>(
                    (static_cast<std::uint32_t>(x_magnitude) << 8U) / z_magnitude);
                projected_y = static_cast<std::uint16_t>(
                    (static_cast<std::uint32_t>(y_magnitude) << 8U) / z_magnitude);
            }
        }
        const auto signed_projection = [original_z](
            std::uint16_t projected, std::int16_t coordinate) {
            const auto negative = (coordinate < 0) != (original_z < 0);
            return negative
                ? wrap16(-static_cast<std::int32_t>(projected))
                : static_cast<std::int16_t>(projected);
        };
        const auto projected_x_word = add16(
            signed_projection(projected_x, x_word),
            rounded_word(vanish_x + stereo_shift));
        const auto projected_y_word = add16(
            signed_projection(projected_y, y_word),
            rounded_word(vanish_y));
        return {static_cast<double>(projected_x_word),
            static_cast<double>(projected_y_word), point.z, original_z >= 0};
    }
    const auto projection_z = point.z == 0.0 ? 1.0 : point.z;
    const auto projected_x = projection_x * focal_length / projection_z;
    const auto projected_y = point.y * focal_length / projection_z;
    return {
        vanish_x + stereo_shift + (subpixel ? projected_x : std::trunc(projected_x)),
        vanish_y + (subpixel ? projected_y : std::trunc(projected_y)),
        point.z,
        point.z >= 0.0,
    };
}

// MDO_PROJECT for an already transformed signed word. Test saturation before
// division, then divide each axis once. The reference first divides the
// dominant axis to test saturation and divides it again in the ordinary case.
WordProjection project_word(std::int16_t x, std::int16_t y, std::int16_t z,
    std::int16_t vanish_x, std::int16_t vanish_y) {
    const auto ax = std::uint32_t(x < 0 ? -std::int32_t(x) : x);
    const auto ay = std::uint32_t(y < 0 ? -std::int32_t(y) : y);
    const auto az = std::uint32_t(z == 0 ? 1 : z < 0 ? -std::int32_t(z) : z);
    std::uint32_t px{}, py{};
    const auto dominant = std::max(ax, ay);
    if (dominant >= az * 64U) {
        const auto minor = std::min(ax, ay) * 16383U / dominant;
        px = ax >= ay ? 16383U : minor;
        py = ax >= ay ? minor : 16383U;
    } else {
        px = (ax << 8U) / az;
        py = (ay << 8U) / az;
    }
    const auto signed_word = [z](std::uint32_t magnitude, std::int16_t coordinate) {
        return wrap16((coordinate < 0) != (z < 0)
            ? -std::int32_t(magnitude) : std::int32_t(magnitude));
    };
    return {add16(signed_word(px, x), vanish_x),
        add16(signed_word(py, y), vanish_y), z};
}

bool word_visibility(const assets::Visibility& visibility,
    const std::vector<WordProjection>& projected) {
    if (visibility.a >= projected.size() || visibility.b >= projected.size()
        || visibility.c >= projected.size()) return false;
    const auto& a = projected[visibility.a];
    const auto& b = projected[visibility.b];
    const auto& c = projected[visibility.c];
    const auto bx = subtract16(b.x, a.x);
    const auto by = subtract16(b.y, a.y);
    const auto cx = subtract16(c.x, a.x);
    const auto cy = subtract16(c.y, a.y);
    // Unsigned subtraction expresses the source's wrapping long product,
    // without a signed-overflow dependency or a 64-bit multiply on ARM11.
    const auto area = std::uint32_t(std::int32_t(bx) * cy)
        - std::uint32_t(std::int32_t(by) * cx);
    return ((area >> 31U) != 0U) != ((a.z < 0) != ((b.z < 0) != (c.z < 0)));
}

void clip_word_polygon(std::vector<RasterPoint>& polygon,
    std::vector<RasterPoint>& scratch, std::int16_t width, std::int16_t height) {
    // Same four planes, exclusive right/bottom and inside-anchored signed
    // divisions as clip_screen_polygon. No texture coordinates are needed
    // for this solid-face path. A fully inside polygon needs no copies.
    const auto clip = [&](bool vertical, std::int16_t boundary, bool keep_less) {
        const auto coordinate = [vertical](const RasterPoint& p) {
            return std::int16_t(vertical ? p.x : p.y);
        };
        const auto inside = [&](const RasterPoint& p) {
            const auto v = coordinate(p);
            return keep_less ? v < boundary : v >= boundary;
        };
        if (std::all_of(polygon.begin(), polygon.end(), inside)) return;
        scratch.clear();
        scratch.reserve(polygon.size() + 2U);
        auto previous = polygon.back();
        auto previous_inside = inside(previous);
        for (const auto& current : polygon) {
            const auto current_inside = inside(current);
            if (current_inside != previous_inside) {
                const auto& anchor = previous_inside ? previous : current;
                const auto& outside = previous_inside ? current : previous;
                const auto anchor_axis = coordinate(anchor);
                const auto numerator = subtract16(boundary, anchor_axis);
                const auto denominator = subtract16(coordinate(outside), anchor_axis);
                const auto from = std::int16_t(vertical ? anchor.y : anchor.x);
                const auto to = std::int16_t(vertical ? outside.y : outside.x);
                const auto difference = subtract16(to, from);
                const auto value = denominator == 0 ? from : add16(from,
                    wrap16(std::int32_t(numerator) * difference / denominator));
                scratch.push_back(vertical ? RasterPoint{boundary, value}
                    : RasterPoint{value, boundary});
            }
            if (current_inside) scratch.push_back(current);
            previous = current;
            previous_inside = current_inside;
        }
        polygon.swap(scratch);
    };
    clip(true, 0, false);
    clip(true, width, true);
    clip(false, 0, false);
    clip(false, height, true);
}

bool source_visibility(
    const assets::Visibility& visibility,
    const std::vector<ScreenPoint>& projected,
    const std::vector<Vec3>& transformed) {
    if (visibility.a >= projected.size() || visibility.b >= projected.size()
        || visibility.c >= projected.size()) {
        return false;
    }
    const auto& a = projected[visibility.a];
    const auto& b = projected[visibility.b];
    const auto& c = projected[visibility.c];
    // MSH_VIZIS performs all four projected differences in 16-bit Super FX
    // registers, then subtracts the two signed 16x16 products as a wrapping
    // 32-bit value. Host doubles here would give a different facing result
    // whenever a near-plane projection crosses the signed screen-word seam;
    // the intro lasers would then expose a broad side face as a full-screen
    // wedge, especially while Original Speed holds that source frame longer.
    const auto bx = subtract16(
        rounded_word(b.x), rounded_word(a.x));
    const auto by = subtract16(
        rounded_word(b.y), rounded_word(a.y));
    const auto cx = subtract16(
        rounded_word(c.x), rounded_word(a.x));
    const auto cy = subtract16(
        rounded_word(c.y), rounded_word(a.y));
    const auto cross = static_cast<std::int64_t>(bx) * cy
        - static_cast<std::int64_t>(by) * cx;
    const auto signed_area = std::bit_cast<std::int32_t>(
        static_cast<std::uint32_t>(cross));
    const auto odd_behind = (transformed[visibility.a].z < 0.0)
        != ((transformed[visibility.b].z < 0.0)
            != (transformed[visibility.c].z < 0.0));
    return (signed_area < 0) != odd_behind;
}

bool continuous_visibility(
    const assets::Visibility& visibility,
    const std::vector<Vec3>& transformed) {
    if (visibility.a >= transformed.size()
        || visibility.b >= transformed.size()
        || visibility.c >= transformed.size()) {
        return false;
    }
    const auto& a = transformed[visibility.a];
    const auto& b = transformed[visibility.b];
    const auto& c = transformed[visibility.c];
    // The signed projected area, after source_visibility's odd-behind
    // correction, has the same sign as this camera-space determinant. Use
    // that equivalent form for Render Upscale so a visibility triple close
    // to z=0 cannot wrap or quantize through a different facing for one host
    // presentation. At the exact tangent the projected source area is
    // undefined/zero; retaining the face prevents a one-frame hole between
    // the two otherwise-visible neighbouring poses.
    const auto determinant =
        a.x * (b.y * c.z - b.z * c.y)
        - a.y * (b.x * c.z - b.z * c.x)
        + a.z * (b.x * c.y - b.y * c.x);
    const auto coordinate_scale = std::max({
        std::abs(a.x), std::abs(a.y), std::abs(a.z),
        std::abs(b.x), std::abs(b.y), std::abs(b.z),
        std::abs(c.x), std::abs(c.y), std::abs(c.z), 1.0});
    const auto tangent_tolerance = coordinate_scale * coordinate_scale
        * coordinate_scale * 1.0e-12;
    return determinant <= tangent_tolerance;
}

RasterVertex interpolate_at(
    const RasterVertex& a, const RasterVertex& b, double amount) {
    return {
        {
            a.point.x + (b.point.x - a.point.x) * amount,
            a.point.y + (b.point.y - a.point.y) * amount,
            a.point.z + (b.point.z - a.point.z) * amount,
            true,
        },
        {
            a.texture.u + (b.texture.u - a.texture.u) * amount,
            a.texture.v + (b.texture.v - a.texture.v) * amount,
        },
    };
}

template <typename Inside, typename Intersect>
void clip_screen_edge(
    const std::vector<RasterVertex>& input,
    std::vector<RasterVertex>& output,
    Inside inside,
    Intersect intersect) {
    output.clear();
    if (input.empty()) return;
    output.reserve(input.size() + 2U);
    auto previous = input.back();
    auto previous_inside = inside(previous);
    for (const auto& current : input) {
        const auto current_inside = inside(current);
        if (current_inside != previous_inside) {
            output.push_back(intersect(previous, current));
        }
        if (current_inside) output.push_back(current);
        previous = current;
        previous_inside = current_inside;
    }
}

// Scan conversion is the only stage that follows the render scale. Projection,
// visibility and clipping all stay on the source raster, so a scaled frame
// resolves exactly the same geometry with finer scan lines.
class StoredRasterScope {
public:
    StoredRasterScope(Framebuffer& target, std::uint32_t scale) noexcept
        : target_(target), previous_(target.draw_scale()) {
        if (scale > 1U) target_.set_draw_scale(1U);
    }
    ~StoredRasterScope() { target_.set_draw_scale(previous_); }
    StoredRasterScope(const StoredRasterScope&) = delete;
    StoredRasterScope& operator=(const StoredRasterScope&) = delete;

private:
    Framebuffer& target_;
    std::uint32_t previous_;
};

void scale_to_stored(ScreenPoint& point, std::uint32_t scale) noexcept {
    if (scale <= 1U) return;
    point.x *= static_cast<double>(scale);
    point.y *= static_cast<double>(scale);
}

std::vector<RasterVertex> clip_screen_polygon(
    std::vector<RasterVertex> polygon,
    const Framebuffer& target,
    bool source_exact,
    std::vector<RasterVertex>& scratch) {
    // MOBJ.MC stores m_xright/m_ybot as the first coordinates outside the
    // viewport (224/192), and MCLIP.MC intersects edges with those values.
    // Keeping the intersection on that exclusive plane is important because
    // the source scan converter omits its terminal bottom scanline.
    const auto right = static_cast<double>(target.width());
    const auto bottom = static_cast<double>(target.height());
    if (source_exact) {
        const auto source_value_at = [](std::int16_t inside_value,
                                         std::int16_t outside_value,
                                         std::int16_t inside_axis,
                                         std::int16_t outside_axis,
                                         std::int16_t boundary) {
            const auto numerator = subtract16(
                boundary, inside_axis);
            const auto denominator = subtract16(
                outside_axis, inside_axis);
            const auto difference = subtract16(
                outside_value, inside_value);
            if (denominator == 0) return inside_value;
            const auto quotient = static_cast<std::int32_t>(numerator)
                * static_cast<std::int32_t>(difference) / denominator;
            return add16(inside_value,
                wrap16(quotient));
        };
        const auto clip_edge = [&source_value_at, &scratch](
                                   std::vector<RasterVertex>& input,
                                   bool vertical,
                                   std::int16_t boundary,
                                   bool keep_less) {
            scratch.clear();
            if (input.empty()) return;
            auto& output = scratch;
            output.reserve(input.size() + 2U);
            const auto coordinate = [vertical](const RasterVertex& vertex) {
                return rounded_word(vertical
                    ? vertex.point.x : vertex.point.y);
            };
            const auto inside = [&coordinate, boundary, keep_less](
                                    const RasterVertex& vertex) {
                const auto value = coordinate(vertex);
                return keep_less ? value < boundary : value >= boundary;
            };
            auto previous = input.back();
            auto previous_inside = inside(previous);
            for (const auto& current : input) {
                const auto current_inside = inside(current);
                if (current_inside != previous_inside) {
                    // MCLIP.MC's *2 entry points swap an outside->inside
                    // edge, so every signed division is anchored at the
                    // endpoint that remains inside the clip window.
                    const auto& anchor = previous_inside ? previous : current;
                    const auto& outside = previous_inside ? current : previous;
                    const auto anchor_axis = coordinate(anchor);
                    const auto outside_axis = coordinate(outside);
                    auto intersection = anchor;
                    if (vertical) {
                        intersection.point.x = boundary;
                        intersection.point.y = source_value_at(
                            rounded_word(anchor.point.y),
                            rounded_word(outside.point.y),
                            anchor_axis, outside_axis, boundary);
                    } else {
                        intersection.point.x = source_value_at(
                            rounded_word(anchor.point.x),
                            rounded_word(outside.point.x),
                            anchor_axis, outside_axis, boundary);
                        intersection.point.y = boundary;
                    }
                    intersection.texture.u = source_value_at(
                        rounded_word(anchor.texture.u),
                        rounded_word(outside.texture.u),
                        anchor_axis, outside_axis, boundary);
                    intersection.texture.v = source_value_at(
                        rounded_word(anchor.texture.v),
                        rounded_word(outside.texture.v),
                        anchor_axis, outside_axis, boundary);
                    intersection.point.visible = true;
                    output.push_back(intersection);
                }
                if (current_inside) output.push_back(current);
                previous = current;
                previous_inside = current_inside;
            }
            input.swap(output);
        };
        clip_edge(polygon, true, 0, false);
        clip_edge(polygon, true,
            static_cast<std::int16_t>(target.width()), true);
        clip_edge(polygon, false, 0, false);
        clip_edge(polygon, false,
            static_cast<std::int16_t>(target.height()), true);
        return polygon;
    }
    clip_screen_edge(polygon, scratch,
        [](const RasterVertex& vertex) { return vertex.point.x >= 0.0; },
        [](const RasterVertex& a, const RasterVertex& b) {
            const auto denominator = b.point.x - a.point.x;
            return interpolate_at(a, b,
                denominator == 0.0 ? 0.0 : -a.point.x / denominator);
        });
    polygon.swap(scratch);
    clip_screen_edge(polygon, scratch,
        [right](const RasterVertex& vertex) { return vertex.point.x < right; },
        [right](const RasterVertex& a, const RasterVertex& b) {
            const auto denominator = b.point.x - a.point.x;
            return interpolate_at(a, b, denominator == 0.0 ? 0.0
                : (right - a.point.x) / denominator);
        });
    polygon.swap(scratch);
    clip_screen_edge(polygon, scratch,
        [](const RasterVertex& vertex) { return vertex.point.y >= 0.0; },
        [](const RasterVertex& a, const RasterVertex& b) {
            const auto denominator = b.point.y - a.point.y;
            return interpolate_at(a, b,
                denominator == 0.0 ? 0.0 : -a.point.y / denominator);
        });
    polygon.swap(scratch);
    clip_screen_edge(polygon, scratch,
        [bottom](const RasterVertex& vertex) { return vertex.point.y < bottom; },
        [bottom](const RasterVertex& a, const RasterVertex& b) {
            const auto denominator = b.point.y - a.point.y;
            return interpolate_at(a, b, denominator == 0.0 ? 0.0
                : (bottom - a.point.y) / denominator);
        });
    polygon.swap(scratch);
    return polygon;
}

bool clip_screen_line(
    ScreenPoint& a,
    ScreenPoint& b,
    const Framebuffer& target,
    bool source_exact) {
    if (source_exact) {
        const auto clip_edge = [&a, &b](
                                   bool vertical,
                                   std::int16_t boundary,
                                   bool keep_less) {
            const auto coordinate = [vertical](const ScreenPoint& point) {
                return rounded_word(vertical ? point.x : point.y);
            };
            const auto inside = [keep_less, boundary](std::int16_t value) {
                return keep_less ? value < boundary : value >= boundary;
            };
            const auto a_axis = coordinate(a);
            const auto b_axis = coordinate(b);
            const auto a_inside = inside(a_axis);
            const auto b_inside = inside(b_axis);
            if (!a_inside && !b_inside) return false;
            if (a_inside && b_inside) return true;
            auto& outside = a_inside ? b : a;
            const auto& anchor = a_inside ? a : b;
            const auto anchor_axis = coordinate(anchor);
            const auto outside_axis = coordinate(outside);
            const auto denominator = subtract16(
                outside_axis, anchor_axis);
            const auto interpolate = [anchor_axis, boundary,
                                      denominator](double inside_value,
                                                   double outside_value) {
                if (denominator == 0) return rounded_word(inside_value);
                const auto numerator = subtract16(
                    boundary, anchor_axis);
                const auto difference = subtract16(
                    rounded_word(outside_value), rounded_word(inside_value));
                const auto quotient = static_cast<std::int32_t>(numerator)
                    * static_cast<std::int32_t>(difference) / denominator;
                return add16(
                    rounded_word(inside_value),
                    wrap16(quotient));
            };
            if (vertical) {
                outside.x = boundary;
                outside.y = interpolate(anchor.y, outside.y);
            } else {
                outside.x = interpolate(anchor.x, outside.x);
                outside.y = boundary;
            }
            outside.visible = true;
            return true;
        };
        return clip_edge(true, 0, false)
            && clip_edge(true, static_cast<std::int16_t>(target.width()), true)
            && clip_edge(false, 0, false)
            && clip_edge(false, static_cast<std::int16_t>(target.height()), true);
    }
    auto first = RasterVertex{a, {}};
    auto second = RasterVertex{b, {}};
    auto enter = 0.0;
    auto leave = 1.0;
    const auto dx = second.point.x - first.point.x;
    const auto dy = second.point.y - first.point.y;
    const auto clip = [&enter, &leave](double direction, double distance) {
        if (direction == 0.0) return distance >= 0.0;
        const auto amount = distance / direction;
        if (direction < 0.0) {
            if (amount > leave) return false;
            enter = std::max(enter, amount);
        } else {
            if (amount < enter) return false;
            leave = std::min(leave, amount);
        }
        return true;
    };
    const auto right = static_cast<double>(target.width());
    const auto bottom = static_cast<double>(target.height());
    if (!clip(-dx, first.point.x)
        || !clip(dx, right - first.point.x)
        || !clip(-dy, first.point.y)
        || !clip(dy, bottom - first.point.y)
        || enter > leave) {
        return false;
    }
    const auto original_first = first;
    const auto original_second = second;
    first = interpolate_at(original_first, original_second, enter);
    second = interpolate_at(original_first, original_second, leave);
    a = first.point;
    b = second.point;
    return true;
}

// `subpixel` is the pose's subpixel_projection as the caller rasterizes it
// (draw_impl also selects it for continuous geometry).
Vec3 rotate_with_factor(
    const assets::Vec3i& point, const RenderPose& pose, double factor,
    bool subpixel) {
    Vec3 value{point.x * factor, point.y * factor, point.z * factor};
    if (pose.use_rotation_matrix) {
        if (subpixel) {
            constexpr auto q15 = 32'768.0;
            const auto transform = [&pose, &value](std::size_t column) {
                return value.x * pose.rotation_matrix[column] / q15
                    + value.y * pose.rotation_matrix[3U + column] / q15
                    + value.z * pose.rotation_matrix[6U + column] / q15;
            };
            value = {transform(0), transform(1), transform(2)};
            value.x += pose.x;
            value.y += pose.y;
            value.z += pose.z;
            return value;
        }
        const auto transform = [&pose, &value](std::size_t column) {
            const auto x = wrap16(
                static_cast<std::int64_t>(exact_lround(value.x)));
            const auto y = wrap16(
                static_cast<std::int64_t>(exact_lround(value.y)));
            const auto z = wrap16(
                static_cast<std::int64_t>(exact_lround(value.z)));
            auto result = multiply_q15(
                x, pose.rotation_matrix[column]);
            result = add16(result,
                multiply_q15(
                    y, pose.rotation_matrix[3U + column]));
            result = add16(result,
                multiply_q15(
                    z, pose.rotation_matrix[6U + column]));
            return static_cast<double>(result);
        };
        value = {transform(0), transform(1), transform(2)};
        value.x = add16(rounded_word(value.x),
            rounded_word(pose.x));
        value.y = add16(rounded_word(value.y),
            rounded_word(pose.y));
        value.z = add16(rounded_word(value.z),
            rounded_word(pose.z));
        return value;
    }
    const auto pitch = pose.pitch * kTau / 65'536.0;
    const auto yaw = pose.yaw * kTau / 65'536.0;
    const auto roll = pose.roll * kTau / 65'536.0;

    const auto cx = std::cos(pitch);
    const auto sx = std::sin(pitch);
    const auto cy = std::cos(yaw);
    const auto sy = std::sin(yaw);
    const auto cz = std::cos(roll);
    const auto sz = std::sin(roll);

    value = {value.x, value.y * cx - value.z * sx, value.y * sx + value.z * cx};
    value = {value.x * cy + value.z * sy, value.y, -value.x * sy + value.z * cy};
    value = {value.x * cz - value.y * sz, value.x * sz + value.y * cz, value.z};
    value.x += pose.x;
    value.y += pose.y;
    value.z += pose.z;
    return value;
}

Vec3 rotate(const assets::Vec3i& point, const RenderPose& pose,
    std::uint8_t shift) {
    const auto factor = pose.scale
        * static_cast<double>(std::uint32_t{1} << shift);
    return rotate_with_factor(point, pose, factor, pose.subpixel_projection);
}

Vec3 explosion_offset(
    const assets::Face& face, const RenderPose& pose) {
    if (pose.explosion_progress == 0U) return {};
    auto direction_pose = pose;
    direction_pose.x = 0.0;
    direction_pose.y = 0.0;
    direction_pose.z = 0.0;
    direction_pose.scale = 1.0;
    // MOBJ's mexpfacesinit negates the source X/Z face-normal bytes, rotates
    // that vector through m_mat, then forces Y downward before multiplying by
    // al_count and dividing by four.
    auto direction = rotate(
        {-face.normal.x, face.normal.y, -face.normal.z}, direction_pose, 0U);
    direction.y = -std::abs(direction.y);
    const auto expand = [progress = pose.explosion_progress](double value) {
        const auto product = static_cast<std::int32_t>(exact_lround(value))
            * static_cast<std::int32_t>(progress);
        return static_cast<double>(
            arithmetic_shift_right(product, 2U));
    };
    return {expand(direction.x), expand(direction.y), expand(direction.z)};
}

constexpr auto kFastRasterSpan = 400U;
constexpr auto kQ15SpanReciprocals = [] {
    std::array<std::int16_t, kFastRasterSpan + 1U> values{};
    values[1] = 32'767;
    for (std::size_t span = 2U; span < values.size(); ++span) {
        values[span] = static_cast<std::int16_t>(
            32'768 / static_cast<std::int32_t>(span));
    }
    return values;
}();

std::int32_t q15_span_reciprocal(std::int32_t span) noexcept {
    if (span <= 0) return 0;
    if (static_cast<std::uint32_t>(span) < kQ15SpanReciprocals.size())
        return kQ15SpanReciprocals[static_cast<std::size_t>(span)];
    return span == 1 ? 32'767 : 32'768 / span;
}

std::size_t advance_polygon_vertex(
    std::size_t vertex, int direction, std::size_t count) noexcept {
    if (direction > 0) {
        const auto next = vertex + 1U;
        return next == count ? 0U : next;
    }
    return vertex == 0U ? count - 1U : vertex - 1U;
}

std::int32_t wide_edge_increment(
    std::int32_t difference, std::int32_t scanlines) noexcept {
    if (scanlines <= 0) return 0;
    const auto reciprocal = q15_span_reciprocal(scanlines);
    return arithmetic_shift_right(
        static_cast<std::int32_t>(difference * reciprocal), 7);
}

std::int16_t source_edge_increment(
    std::int32_t difference, std::int32_t scanlines) noexcept {
    return wrap16(
        wide_edge_increment(difference, scanlines));
}

std::int16_t source_advance_edge(
    std::int16_t position, std::int16_t increment) noexcept {
    auto advanced = add16(position, increment);
    // MDRAWP.MC's deliberate workaround catches only the narrow wrapped
    // interval immediately below x=0. Valid clipped x>=128 also has bit 15
    // set, but remains negative after adding $0800 and is left untouched.
    if (advanced < 0
        && add16(advanced, 0x0800) >= 0) {
        advanced = 0;
    }
    return advanced;
}

std::int32_t source_fixed_integer(std::int16_t value) noexcept {
    return static_cast<std::uint16_t>(value) >> 8U;
}

// Records one span as a command when the target records at draw scale 1;
// otherwise returns false, recording nothing, so the caller draws it.
bool record_span(Framebuffer& target,int left,int right,int y,FaceColour colour,
    std::uint8_t base,SurfaceBuffer* surfaces,const SurfaceSample& surface) {
    auto* commands=target.command_buffer();
    if(!commands || target.draw_scale()!=1) return false;
    RasterCommand command;
    command.left=left;command.right=right+1;command.top=y;command.bottom=y+1;
    command.even=std::uint8_t(base+colour.even);command.odd=std::uint8_t(base+colour.odd);
    command.dither=colour.dither;command.tag=target.layer_override()<0?1:target.layer_override();
    command.has_surface=surfaces!=nullptr;
    command.surface={surface.normal_x,surface.normal_y,surface.normal_z,surface.depth};
    commands->add(command);return true;
}

// fill_source_polygon_points for the one case every solid cartridge face of
// the native world takes: a recorder on an unscaled raster wider than the
// cartridge's 224 pixels (unwrapped X), the retail fill (no wobble,
// wireframe, cel or wave variant), source winding and no surface buffer.
// The tracers, their arithmetic and the commands are those of the general
// routine below, statement for statement, without its per-row dispatch over
// the variants and with each command built in place.
void record_plain_polygon(
    RasterCommands& commands,
    std::span<const RasterPoint> points,
    FaceColour colour,
    std::uint8_t colour_index_base,
    std::uint32_t tag,
    const SurfaceSample& surface) {
    const auto count = points.size();
    auto minimum = std::size_t{};
    auto maximum_y = points.front().y;
    for (std::size_t index = 1; index < count; ++index) {
        if (points[index].y < points[minimum].y) minimum = index;
        maximum_y = std::max(maximum_y, points[index].y);
    }
    auto y = points[minimum].y;
    if (y == maximum_y) return;

    struct Tracer {
        std::size_t vertex{};
        int direction{};
        std::int32_t x{};
        std::int32_t increment{};
        std::int32_t remaining{};
    };
    const auto initial_x = points[minimum].x << 8U;
    Tracer left{minimum, 1, initial_x, 0, 0};
    Tracer right{minimum, -1, initial_x, 0, 0};
    const auto begin_segment = [&points, &y, count](Tracer& tracer) {
        auto rounded = (tracer.x + 127) >> 8;
        for (std::size_t guard = 0; guard < count; ++guard) {
            tracer.vertex = advance_polygon_vertex(
                tracer.vertex, tracer.direction, count);
            const auto& endpoint = points[tracer.vertex];
            const auto scanlines = endpoint.y - y;
            if (scanlines < 0) return false;
            if (scanlines == 0) {
                rounded = endpoint.x;
                tracer.x = rounded << 8U;
                continue;
            }
            tracer.x = rounded << 8U;
            tracer.increment = wide_edge_increment(
                endpoint.x - rounded, scanlines);
            tracer.remaining = scanlines;
            return true;
        }
        return false;
    };

    const std::uint32_t even = static_cast<std::uint8_t>(colour_index_base + colour.even);
    const std::uint32_t odd = static_cast<std::uint8_t>(colour_index_base + colour.odd);
    const std::uint32_t dither = colour.dither;
    const bool edge_runs = commands.accepts_edge_runs();
    // A span stores the caller's surface sample. Every caller of the plain
    // fill passes the default one (normal +Z), which the recorder can pack.
    const std::array<float, 4> span_surface{surface.normal_x, surface.normal_y,
        surface.normal_z, surface.depth};
    constexpr std::array<float, 4> default_surface{0.0F, 0.0F, 1.0F, 0.0F};
    const bool packable_surface = std::memcmp(span_surface.data(),
        default_surface.data(), sizeof(span_surface)) == 0;
    while (y < maximum_y) {
        if (left.remaining == 0 && !begin_segment(left)) return;
        if (right.remaining == 0 && !begin_segment(right)) return;
        if (edge_runs) {
            const auto rows = std::min(left.remaining, right.remaining);
            const auto left_last = left.x + left.increment * (rows - 1);
            const auto right_last = right.x + right.increment * (rows - 1);
            if (rows > 1 && right.x >= left.x && right_last >= left_last) {
                commands.add_edge_run(
                    std::min(left.x >> 8, left_last >> 8), y,
                    std::max(right.x >> 8, right_last >> 8) + 1, y + rows,
                    even, odd, dither, tag,
                    left.x, right.x, left.increment, right.increment);
                left.x += left.increment * rows;
                right.x += right.increment * rows;
                left.remaining -= rows;
                right.remaining -= rows;
                y += rows;
                continue;
            }
        }
        // A single row, or sides that cross: one span at a time, with the
        // surface sample record_span stores (an edge run stores none).
        const auto x1 = left.x >> 8;
        const auto x2 = right.x >> 8;
        if (x2 >= x1) {
            if (packable_surface) {
                commands.add_solid(x1, y, x2 + 1, y + 1, even, odd, dither, tag, true);
            } else {
                RasterCommand command;
                command.left = x1; command.right = x2 + 1;
                command.top = y; command.bottom = y + 1;
                command.even = even; command.odd = odd;
                command.dither = dither; command.tag = tag;
                command.surface = span_surface;
                commands.add(command);
            }
        }
        left.x += left.increment;
        right.x += right.increment;
        --left.remaining;
        --right.remaining;
        ++y;
    }
}

void fill_source_polygon_points(
    Framebuffer& target,
    std::span<const RasterPoint> points,
    FaceColour colour,
    std::uint8_t colour_index_base,
    const RenderPose& pose,
    bool winding_independent,
    SurfaceBuffer* surfaces,
    const SurfaceSample& surface) {
    if (points.size() < 3U) return;
    // The native world's solid faces: see record_plain_polygon.
    if (auto* const recorder = target.command_buffer(); recorder != nullptr
        && !winding_independent && surfaces == nullptr
        && target.draw_scale() == 1U && target.width() > 224U
        && pose.wobble_mode == 0U && pose.wireframe_mode == 0U
        && !pose.cel_mode && !pose.wave_mode) {
        record_plain_polygon(*recorder, points, colour, colour_index_base,
            target.layer_override() < 0 ? 1U
                : static_cast<std::uint32_t>(target.layer_override()), surface);
        return;
    }
    auto minimum = std::size_t{};
    auto maximum_y = points.front().y;
    for (std::size_t index = 1; index < points.size(); ++index) {
        if (points[index].y < points[minimum].y) minimum = index;
        maximum_y = std::max(maximum_y, points[index].y);
    }
    auto y = points[minimum].y;
    if (y == maximum_y) return;

    // The source scan converter keeps X in an unsigned 8.8 word. That covers
    // the retail 0..224 clip window, but an exclusive X=256 boundary wraps to
    // zero, while the source's -8..0 overflow guard mistakes valid X=248..255
    // for a negative edge. Use an unwrapped host accumulator only when the
    // viewport is wider than the cartridge raster; the 224-pixel path below
    // retains the exact word arithmetic used by MDRAWP.MC.
    const auto extended_x = target.width() > 224U;
    const auto fixed_x = [extended_x](std::int32_t value) {
        return extended_x
            ? value << 8U
            : static_cast<std::int32_t>(wrap16(
                static_cast<std::int64_t>(value) << 8U));
    };
    const auto integer_x = [extended_x](std::int32_t value) {
        return extended_x
            ? arithmetic_shift_right(value, 8)
            : source_fixed_integer(static_cast<std::int16_t>(value));
    };
    const auto rounded_x = [extended_x, &integer_x](std::int32_t value) {
        return extended_x
            ? arithmetic_shift_right(value + 127, 8)
            : integer_x(add16(
                static_cast<std::int16_t>(value), 127));
    };
    const auto advance_x = [extended_x](
                               std::int32_t value, std::int32_t increment) {
        return extended_x
            ? value + increment
            : static_cast<std::int32_t>(source_advance_edge(
                static_cast<std::int16_t>(value),
                static_cast<std::int16_t>(increment)));
    };
    const auto edge_increment = [extended_x](
                                    std::int32_t difference,
                                    std::int32_t scanlines) {
        return extended_x
            ? wide_edge_increment(difference, scanlines)
            : static_cast<std::int32_t>(
                source_edge_increment(difference, scanlines));
    };

    struct Tracer {
        std::size_t vertex{};
        int direction{};
        std::int32_t x{};
        std::int32_t increment{};
        std::int32_t remaining{};
    };
    const auto initial_x = fixed_x(points[minimum].x);
    Tracer left{minimum, 1, initial_x, 0, 0};
    Tracer right{minimum, -1, initial_x, 0, 0};
    const auto begin_segment = [
                                   &points, &y, &fixed_x, &rounded_x,
                                   &edge_increment](Tracer& tracer) {
        const auto count = points.size();
        auto rounded = rounded_x(tracer.x);
        for (std::size_t guard = 0; guard < count; ++guard) {
            tracer.vertex = advance_polygon_vertex(
                tracer.vertex, tracer.direction, count);
            const auto& endpoint = points[tracer.vertex];
            const auto scanlines = endpoint.y - y;
            if (scanlines < 0) return false;
            if (scanlines == 0) {
                rounded = endpoint.x;
                tracer.x = fixed_x(rounded);
                continue;
            }
            tracer.x = fixed_x(rounded);
            tracer.increment = edge_increment(
                endpoint.x - rounded, scanlines);
            tracer.remaining = scanlines;
            return true;
        }
        return false;
    };

    constexpr std::array<std::int8_t, 32> wave_sine{
        0, 1, 2, 3, 3, 3, 2, 1,
        0, -1, -2, -3, -3, -3, -2, -1,
        0, 1, 2, 3, 3, 3, 2, 1,
        0, -1, -2, -3, -3, -3, -2, -1,
    };
    const auto wave_y = [&pose, &wave_sine](
                            std::int32_t x, std::int32_t scanline) {
        auto phase = wrap16(
            static_cast<std::int32_t>(pose.wave_offset) + x);
        phase = wrap16(
            arithmetic_shift_right(phase, 1)
            + static_cast<std::int32_t>(pose.animation_frame & 15U) - 1);
        auto index = static_cast<std::int32_t>(phase);
        index %= static_cast<std::int32_t>(wave_sine.size());
        if (index < 0) index += static_cast<std::int32_t>(wave_sine.size());
        return scanline + wave_sine[static_cast<std::size_t>(index)];
    };
    auto mode2_edge_continuation = false;
    auto previous_wobble_left = std::int32_t{};
    auto has_previous_wobble_left = false;

    // Until one of them ends, both sides advance by a constant step. A
    // recorder that draws slanted sides takes that whole stretch of plain
    // spans as one edge run; its rows are exactly the spans it replaces.
    auto* const run_commands = target.command_buffer();
    const bool edge_runs = run_commands != nullptr
        && run_commands->accepts_edge_runs() && target.draw_scale() == 1U
        && extended_x && surfaces == nullptr && pose.wobble_mode == 0U
        && pose.wireframe_mode == 0U && !pose.cel_mode && !pose.wave_mode;

    while (y < maximum_y) {
        const auto left_starts_segment = left.remaining == 0;
        const auto right_starts_segment = right.remaining == 0;
        if (left_starts_segment && !begin_segment(left)) return;
        if (right_starts_segment && !begin_segment(right)) return;
        auto x1 = integer_x(left.x);
        auto x2 = integer_x(right.x);
        // Fractional projection and near-plane clipping can reverse the two
        // active edges without changing whether the source visibility plane
        // selected this face. The cartridge raster silently assumes its
        // original winding; at upscale resolution that assumption would
        // discard the complete span, making the face flash off for a
        // presentation.
        if (winding_independent && x2 < x1) std::swap(x1, x2);
        if (edge_runs) {
            const auto rows = std::min(left.remaining, right.remaining);
            const auto left_last = left.x + left.increment * (rows - 1);
            const auto right_last = right.x + right.increment * (rows - 1);
            // The distance between the sides is linear too: with one sign at
            // both ends it keeps it, and every row is the span between the
            // pixels of the lower and the higher side. A source winding
            // that ignores order draws that span either way round.
            const bool forward = right.x >= left.x && right_last >= left_last;
            const bool backward = winding_independent
                && right.x <= left.x && right_last <= left_last;
            if (rows > 1 && (forward || backward)) {
                const auto& low = forward ? left : right;
                const auto low_last = forward ? left_last : right_last;
                const auto& high = forward ? right : left;
                const auto high_last = forward ? right_last : left_last;
                RasterCommand command;
                command.left = std::min(integer_x(low.x), integer_x(low_last));
                command.right = std::max(integer_x(high.x), integer_x(high_last)) + 1;
                command.top = y; command.bottom = y + rows;
                command.even = std::uint8_t(colour_index_base + colour.even);
                command.odd = std::uint8_t(colour_index_base + colour.odd);
                command.dither = colour.dither;
                command.tag = target.layer_override() < 0 ? 1 : target.layer_override();
                command.u = low.x; command.v = high.x;
                command.du = low.increment; command.dv = high.increment;
                command.reserved1 = 8U;
                run_commands->add(command);
                left.x += left.increment * rows;
                right.x += right.increment * rows;
                left.remaining -= rows;
                right.remaining -= rows;
                y += rows;
                continue;
            }
            // A single row, or sides that cross: one span at a time.
            if (x2 >= x1) static_cast<void>(record_span(target, x1, x2, y,
                colour, colour_index_base, surfaces, surface));
        } else if (x2 >= x1) {
            auto raster_row = target.raster_row(y);
            const auto plot = [&](std::int32_t x, std::int32_t plot_y) {
                if(record_span(target,x,x,plot_y,colour,colour_index_base,surfaces,surface)) return;
                const auto palette_index = static_cast<std::uint8_t>(colour_index_base
                    + (colour.dither && ((x ^ plot_y) & 1) != 0
                        ? colour.odd : colour.even));
                if (plot_y == y) raster_row.set(x, palette_index);
                else target.set(x, plot_y, palette_index);
                if (surfaces != nullptr) {
                    surfaces->set(x, plot_y, surface, palette_index);
                }
            };
            const auto span = [&](std::int32_t first, std::int32_t last) {
                if (record_span(target, first, last, y, colour,
                        colour_index_base, surfaces, surface)) return;
                if (surfaces == nullptr && raster_row.fill_span(first, last,
                        static_cast<std::uint8_t>(colour_index_base + colour.even),
                        static_cast<std::uint8_t>(colour_index_base + colour.odd),
                        colour.dither)) return;
                for (auto x = first; x <= last; ++x) plot(x, y);
            };
            // Wobble mode 2 selects hlines22: its span loop deliberately has
            // PLOT commented out. On continuing right-edge segments the
            // mhlines2 entry emits exactly one pixel at the previous left X.
            if ((pose.wobble_mode & 2U) != 0U) {
                if (!right_starts_segment && has_previous_wobble_left) {
                    plot(previous_wobble_left, y);
                }
            // EX hlines23 draws a complete chord whenever either polygon
            // tracer begins a new edge. Its mhlinesA continuation plots only
            // the two edge pixels on all other scanlines. Mode 2 normally
            // fills, but a left-only edge change enters that same mhlinesA
            // continuation until the right tracer begins its next segment.
            } else if ((pose.wireframe_mode == 1U
                    && !left_starts_segment && !right_starts_segment)
                || (pose.wireframe_mode == 2U
                    && mode2_edge_continuation
                    && !left_starts_segment && !right_starts_segment)) {
                plot(x1, y);
                if (x2 != x1) plot(x2, y);
            } else if (pose.cel_mode && pose.wireframe_mode == 0U) {
                // hlines2rr cancels PLOT's automatic X increment and skips
                // the two span endpoints, leaving the source cel outline.
                span(x1 + 1, x2 - 1);
            } else if (pose.wave_mode && pose.wireframe_mode == 0U) {
                for (auto x = x1; x <= x2; ++x) plot(x, wave_y(x, y));
            } else {
                span(x1, x2);
            }
        }
        if ((pose.wobble_mode & 2U) != 0U) {
            previous_wobble_left = x1;
            has_previous_wobble_left = true;
        }
        if (pose.wireframe_mode == 2U) {
            if (right_starts_segment) mode2_edge_continuation = false;
            if (left_starts_segment && !right_starts_segment) {
                mode2_edge_continuation = true;
            }
        }
        left.x = advance_x(left.x, left.increment);
        right.x = advance_x(right.x, right.increment);
        --left.remaining;
        --right.remaining;
        if ((pose.wobble_mode & 1U) != 0U) {
            // The source repeats every step of a trapezoid on the same row;
            // when either tracer expires it advances Y once here and once in
            // the shared normal tail, producing NAN mode 6's two-row jump.
            if (left.remaining != 0 && right.remaining != 0) continue;
            ++y;
        }
        ++y;
    }
}

void fill_source_polygon(
    Framebuffer& target,
    const std::vector<RasterVertex>& polygon,
    FaceColour colour,
    std::uint8_t colour_index_base,
    const RenderPose& pose,
    bool winding_independent,
    SurfaceBuffer* surfaces,
    const SurfaceSample& surface) {
    if (polygon.size() < 3U) return;
    using Point = RasterPoint;
    constexpr std::size_t kInlinePolygonVertices = 16U;
    std::array<Point, kInlinePolygonVertices> inline_points{};
    std::vector<Point> overflow_points;
    std::span<Point> points;
    if (polygon.size() <= inline_points.size()) {
        points = std::span<Point>{inline_points.data(), polygon.size()};
    } else {
        overflow_points.resize(polygon.size());
        points = std::span<Point>{overflow_points};
    }
    for (std::size_t index = 0; index < polygon.size(); ++index) {
        const auto& vertex = polygon[index];
        points[index] = {
            std::clamp<std::int32_t>(static_cast<std::int32_t>(exact_lround(vertex.point.x)),
                0, static_cast<std::int32_t>(target.width())),
            std::clamp<std::int32_t>(static_cast<std::int32_t>(exact_lround(vertex.point.y)),
                0, static_cast<std::int32_t>(target.height())),
        };
    }

    fill_source_polygon_points(target, points, colour, colour_index_base,
        pose, winding_independent, surfaces, surface);
}

// Affine textured fill with the same 8.8 edge tracers as
// fill_source_polygon_points. Texel 0 is transparent; writes are tagged
// textured_geometry.
void fill_source_textured_polygon(
    Framebuffer& target,
    const std::vector<RasterVertex>& polygon,
    const assets::TextureImage& texture,
    std::int32_t texture_scroll_x,
    std::int32_t texture_scroll_y,
    std::uint8_t colour_index_base,
    bool winding_independent,
    SurfaceBuffer* surfaces,
    const SurfaceSample& surface) {
    if (polygon.size() < 3U) return;
    const ScopedLayer texture_layer{target, PixelLayer::textured_geometry};
    struct Point {
        std::int32_t x{};
        std::int32_t y{};
        std::int32_t u{};
        std::int32_t v{};
    };
    constexpr std::size_t kInlinePolygonVertices = 16U;
    std::array<Point, kInlinePolygonVertices> inline_points{};
    std::vector<Point> overflow_points;
    std::span<Point> points;
    if (polygon.size() <= inline_points.size()) {
        points = std::span<Point>{inline_points.data(), polygon.size()};
    } else {
        overflow_points.resize(polygon.size());
        points = std::span<Point>{overflow_points};
    }
    for (std::size_t index = 0; index < polygon.size(); ++index) {
        const auto& vertex = polygon[index];
        points[index] = {
            std::clamp<std::int32_t>(static_cast<std::int32_t>(exact_lround(vertex.point.x)),
                0, static_cast<std::int32_t>(target.width())),
            std::clamp<std::int32_t>(static_cast<std::int32_t>(exact_lround(vertex.point.y)),
                0, static_cast<std::int32_t>(target.height())),
            static_cast<std::int32_t>(exact_lround(vertex.texture.u)),
            static_cast<std::int32_t>(exact_lround(vertex.texture.v)),
        };
    }

    auto minimum = std::size_t{};
    auto maximum_y = points.front().y;
    for (std::size_t index = 1; index < points.size(); ++index) {
        if (points[index].y < points[minimum].y) minimum = index;
        maximum_y = std::max(maximum_y, points[index].y);
    }
    auto y = points[minimum].y;
    if (y == maximum_y) return;
    const auto fixed = [](std::int32_t value) {
        return wrap16(
            static_cast<std::int64_t>(value) << 8U);
    };
    const auto extended_x = target.width() > 224U;
    const auto fixed_x = [extended_x](std::int32_t value) {
        return extended_x
            ? value << 8U
            : static_cast<std::int32_t>(wrap16(
                static_cast<std::int64_t>(value) << 8U));
    };
    const auto integer_x = [extended_x](std::int32_t value) {
        return extended_x
            ? arithmetic_shift_right(value, 8)
            : source_fixed_integer(static_cast<std::int16_t>(value));
    };
    const auto rounded_x = [extended_x, &integer_x](std::int32_t value) {
        return extended_x
            ? arithmetic_shift_right(value + 127, 8)
            : integer_x(add16(
                static_cast<std::int16_t>(value), 127));
    };
    const auto advance_x = [extended_x](
                               std::int32_t value, std::int32_t increment) {
        return extended_x
            ? value + increment
            : static_cast<std::int32_t>(source_advance_edge(
                static_cast<std::int16_t>(value),
                static_cast<std::int16_t>(increment)));
    };
    const auto edge_increment = [extended_x](
                                    std::int32_t difference,
                                    std::int32_t scanlines) {
        return extended_x
            ? wide_edge_increment(difference, scanlines)
            : static_cast<std::int32_t>(
                source_edge_increment(difference, scanlines));
    };

    struct Tracer {
        std::size_t vertex{};
        int direction{};
        std::int32_t x{};
        std::int16_t u{};
        std::int16_t v{};
        std::int32_t x_increment{};
        std::int16_t u_increment{};
        std::int16_t v_increment{};
        std::int32_t remaining{};
    };
    Tracer left{minimum, 1, fixed_x(points[minimum].x),
        fixed(points[minimum].u), fixed(points[minimum].v)};
    Tracer right{minimum, -1, fixed_x(points[minimum].x),
        fixed(points[minimum].u), fixed(points[minimum].v)};
    const auto begin_segment = [
                                   &points, &fixed, &fixed_x, &rounded_x,
                                   &edge_increment, &y](Tracer& tracer) {
        const auto count = points.size();
        auto start_x = rounded_x(tracer.x);
        auto start_u = source_fixed_integer(tracer.u);
        auto start_v = source_fixed_integer(tracer.v);
        for (std::size_t guard = 0; guard < count; ++guard) {
            tracer.vertex = advance_polygon_vertex(
                tracer.vertex, tracer.direction, count);
            const auto& endpoint = points[tracer.vertex];
            const auto scanlines = endpoint.y - y;
            if (scanlines < 0) return false;
            if (scanlines == 0) {
                start_x = endpoint.x;
                start_u = endpoint.u;
                start_v = endpoint.v;
                tracer.x = fixed_x(start_x);
                tracer.u = fixed(start_u);
                tracer.v = fixed(start_v);
                continue;
            }
            tracer.x = fixed_x(start_x);
            tracer.u = fixed(start_u);
            tracer.v = fixed(start_v);
            tracer.x_increment = edge_increment(
                endpoint.x - start_x, scanlines);
            tracer.u_increment = source_edge_increment(
                endpoint.u - start_u, scanlines);
            tracer.v_increment = source_edge_increment(
                endpoint.v - start_v, scanlines);
            tracer.remaining = scanlines;
            return true;
        }
        return false;
    };

    const auto width = static_cast<std::size_t>(texture.u_mask) + 1U;
    while (y < maximum_y) {
        if (left.remaining == 0 && !begin_segment(left)) return;
        if (right.remaining == 0 && !begin_segment(right)) return;
        auto x1 = integer_x(left.x);
        auto x2 = integer_x(right.x);
        const auto next_left_u = source_advance_edge(
            left.u, left.u_increment);
        const auto next_left_v = source_advance_edge(
            left.v, left.v_increment);
        const auto next_right_u = source_advance_edge(
            right.u, right.u_increment);
        const auto next_right_v = source_advance_edge(
            right.v, right.v_increment);
        auto span_u = left.u;
        auto span_v = left.v;
        auto span_right_u = next_right_u;
        auto span_right_v = next_right_v;
        if (winding_independent && x2 < x1) {
            // Keep the corresponding affine texture endpoints attached to
            // whichever edge is now the left side of the stored raster.
            std::swap(x1, x2);
            span_u = right.u;
            span_v = right.v;
            span_right_u = next_left_u;
            span_right_v = next_left_v;
        }
        if (x2 >= x1) {
            const auto span = x2 - x1;
            const auto reciprocal = static_cast<std::int16_t>(
                q15_span_reciprocal(span));
            const auto u_increment = multiply_q15(
                subtract16(span_right_u, span_u), reciprocal);
            const auto v_increment = multiply_q15(
                subtract16(span_right_v, span_v), reciprocal);
            auto u = span_u;
            auto v = span_v;
            auto* commands=target.command_buffer();
            if(commands && target.draw_scale()==1) {
                RasterCommand command;
                command.left=x1;command.right=x2+1;command.top=y;command.bottom=y+1;
                command.tag=std::uint32_t(PixelLayer::textured_geometry);command.textured=1;
                command.texture_offset=commands->texture(texture.texels);
                command.u_mask=texture.u_mask;command.v_mask=texture.v_mask;command.colour_base=colour_index_base;
                command.u=std::uint16_t(u);command.v=std::uint16_t(v);command.du=u_increment;command.dv=v_increment;
                command.reserved0=std::uint32_t(texture_scroll_x);command.reserved1=std::uint32_t(texture_scroll_y);
                command.has_surface=surfaces!=nullptr;
                command.surface={surface.normal_x,surface.normal_y,surface.normal_z,surface.depth};
                commands->add(command);
            } else {
                auto raster_row = target.raster_row(y);
                for (auto x = x1; x <= x2; ++x) {
                    const auto sample_u = (source_fixed_integer(u)
                        + texture_scroll_x) & texture.u_mask;
                    const auto sample_v = (source_fixed_integer(v)
                        + texture_scroll_y) & texture.v_mask;
                    const auto texel = texture.texels[
                        static_cast<std::size_t>(sample_v) * width
                        + static_cast<std::size_t>(sample_u)];
                    if (texel != 0U) {
                        const auto palette_index = static_cast<std::uint8_t>(
                            colour_index_base + texel);
                        raster_row.set(x, palette_index);
                        if (surfaces != nullptr) {
                            surfaces->set(x, y, surface, palette_index);
                        }
                    }
                    u = add16(u, u_increment);
                    v = add16(v, v_increment);
                }
            }
        }
        left.x = advance_x(left.x, left.x_increment);
        left.u = next_left_u;
        left.v = next_left_v;
        right.x = advance_x(right.x, right.x_increment);
        right.u = next_right_u;
        right.v = next_right_v;
        --left.remaining;
        --right.remaining;
        ++y;
    }
}

void draw_line(
    Framebuffer& target,
    ScreenPoint a,
    ScreenPoint b,
    FaceColour colour,
    std::uint8_t colour_index_base,
    std::uint32_t render_scale, std::uint8_t wireframe_thickness) {
    auto x0 = static_cast<int>(exact_lround(a.x));
    auto y0 = static_cast<int>(exact_lround(a.y));
    const auto x1 = static_cast<int>(exact_lround(b.x));
    const auto y1 = static_cast<int>(exact_lround(b.y));
    const auto dx = std::abs(x1 - x0);
    const auto sx = x0 < x1 ? 1 : -1;
    const auto dy = std::abs(y1 - y0);
    const auto sy = y0 < y1 ? 1 : -1;
    const auto dither_scale = static_cast<int>(std::clamp<std::uint32_t>(render_scale, 1U, 4U));
    const auto thickness = static_cast<int>(std::clamp<std::uint32_t>(render_scale, 1U, 4U)
        * std::clamp<unsigned>(wireframe_thickness, 1U, 4U));
    const auto offset = (thickness - 1) / 2;
    if (auto* commands=target.command_buffer(); commands && commands->accepts_edge_runs()) {
        RasterCommand command;
        command.even=std::uint8_t(colour_index_base+colour.even);
        command.odd=std::uint8_t(colour_index_base+colour.odd);
        command.dither=colour.dither;command.reserved0=dither_scale;
        command.tag=target.layer_override()<0?1:target.layer_override();
        source_line_runs(x0,y0,x1,y1,thickness,[&](int left,int top,int right,int bottom) {
            command.left=left;command.top=top;command.right=right;command.bottom=bottom;
            commands->add(command);
        });
        return;
    }
    const auto plot = [&] {
        if(auto* commands=target.command_buffer()) {
            RasterCommand command;
            command.left=x0-offset;command.top=y0-offset;
            command.right=command.left+thickness;command.bottom=command.top+thickness;
            command.even=std::uint8_t(colour_index_base+colour.even);
            command.odd=std::uint8_t(colour_index_base+colour.odd);command.dither=colour.dither;
            command.reserved0=dither_scale;
            command.tag=target.layer_override()<0?1:target.layer_override();
            commands->add(command);return;
        }
        // Keep one logical pixel of wire thickness when the model raster is
        // supersampled. A single stored pixel shrinks to 1/scale on screen.
        for (int row = 0; row < thickness; ++row) {
            for (int column = 0; column < thickness; ++column) {
                const auto x = x0 + column - offset;
                const auto y = y0 + row - offset;
                target.set(x, y, static_cast<std::uint8_t>(colour_index_base
                    + (colour.dither && (((x / dither_scale) ^ (y / dither_scale)) & 1) != 0
                        ? colour.odd : colour.even)));
            }
        }
    };
    // MDRAWC.MC mline uses a major-axis counter and a floor(major/2)
    // accumulator. Its strict-underflow tie rule differs from the common
    // symmetric Bresenham formulation by one pixel on half-slope lines.
    if (dx >= dy) {
        auto error = dx >> 1;
        for (auto count = dx + 1; count != 0; --count) {
            plot();
            error -= dy;
            if (error < 0) {
                error += dx;
                y0 += sy;
            }
            x0 += sx;
        }
    } else {
        auto error = dy >> 1;
        for (auto count = dy + 1; count != 0; --count) {
            plot();
            error -= dx;
            if (error < 0) {
                error += dy;
                x0 += sx;
            }
            y0 += sy;
        }
    }
}

void draw_textured_sprite(
    Framebuffer& target,
    const ScreenPoint& centre,
    const assets::TextureImage& texture,
    std::uint8_t colour_index_base) {
    if (centre.z < 32.0) return;
    const auto source_width = static_cast<std::int32_t>(texture.u_mask) + 1;
    const auto source_height = static_cast<std::int32_t>(texture.v_mask) + 1;
    const auto centre_x = static_cast<std::int32_t>(exact_lround(centre.x));
    const auto centre_y = static_cast<std::int32_t>(exact_lround(centre.y));
    const auto z = static_cast<std::int32_t>(
        static_cast<std::uint16_t>(rounded_word(centre.z)));

    // msh_s_sprite clears m_sprangle, derives source size from m_sprmask,
    // and passes 256 for 32px sheets or 128 for 64px sheets to mshowspr.
    // mshowspr turns that into an 8.8 source-coordinate increment.
    const auto input_scale = source_width == 64 ? 128 : 256;
    auto increment = static_cast<std::int32_t>(
        (static_cast<std::int64_t>(z) * input_scale) >> 8U);
    if (increment == 0) increment = 1;
    if (increment > 32'767) increment = 32'767;
    const auto half_extent = source_width * 128 / increment;
    const auto unclipped_left = centre_x - half_extent;
    const auto unclipped_top = centre_y - half_extent;
    const auto left = std::max<std::int32_t>(0, unclipped_left);
    const auto right = std::min(static_cast<std::int32_t>(target.width()) - 1,
        centre_x + half_extent);
    const auto top = std::max<std::int32_t>(0, unclipped_top);
    const auto bottom = std::min(static_cast<std::int32_t>(target.height()) - 1,
        centre_y + half_extent);
    if (left > right || top > bottom) return;

    // calcxpyp starts at (size/2)<<8 and advances by the effective depth for
    // each destination pixel. Preserve 16-bit register wrapping.
    auto source_y_fixed = wrap16(
        static_cast<std::int64_t>(source_width / 2) * 256
        + static_cast<std::int64_t>(top - centre_y) * increment);
    if(auto* commands=target.command_buffer()) {
        const int scale=int(target.draw_scale());RasterCommand command;
        command.left=left*scale;command.top=top*scale;command.right=(right+1)*scale;command.bottom=(bottom+1)*scale;
        command.textured=3;command.texture_offset=commands->texture(texture.texels);
        command.u_mask=texture.u_mask;command.v_mask=texture.v_mask;command.colour_base=colour_index_base;
        command.u=source_width/2*256+(left-centre_x)*increment;command.v=source_y_fixed;
        command.du=increment;command.dv=scale;command.tag=target.layer_override()<0?1:target.layer_override();
        commands->add(command);return;
    }
    for (auto y = top; y <= bottom; ++y) {
        const auto source_y = static_cast<std::uint32_t>(
            static_cast<std::uint16_t>(source_y_fixed) >> 8U);
        auto source_x_fixed = wrap16(
            static_cast<std::int64_t>(source_width / 2) * 256
            + static_cast<std::int64_t>(left - centre_x) * increment);
        for (auto x = left; x <= right; ++x) {
            const auto source_x = static_cast<std::uint32_t>(
                static_cast<std::uint16_t>(source_x_fixed) >> 8U);
            if (source_x < static_cast<std::uint32_t>(source_width)
                && source_y < static_cast<std::uint32_t>(source_height)) {
                const auto texel = texture.texels[
                    static_cast<std::size_t>(source_y) * source_width + source_x];
                if (texel != 0U) {
                    target.set(x, y, static_cast<std::uint8_t>(
                        colour_index_base + texel));
                }
            }
            source_x_fixed = add16(
                source_x_fixed, static_cast<std::int16_t>(increment));
        }
        source_y_fixed = add16(
            source_y_fixed, static_cast<std::int16_t>(increment));
    }
}

// A whole-object billboard: the texture point-sampled into a square of
// simple_sprite_world_size * focal_length / z source pixels (at most 240)
// around the projected pose, kept inside the pose's effect clip.
void draw_simple_scaled_sprite(
    Framebuffer& target,
    const assets::TextureImage& texture,
    const RenderPose& pose,
    double focal_length,
    std::uint8_t colour_index_base) {
    if (pose.z < 128.0 || pose.simple_sprite_world_size <= 0) return;
    auto dimension = static_cast<int>(std::trunc(
        static_cast<double>(pose.simple_sprite_world_size) * focal_length / pose.z));
    dimension = std::clamp(dimension, 0, 240);
    if (dimension == 0) return;
    const auto centre_x = static_cast<int>(exact_lround(pose.vanish_x))
        + static_cast<int>(std::trunc((pose.x + pose.stereo_eye_offset) * focal_length / pose.z))
        + inward_stereo_shift(pose.stereo_eye_offset, pose.z);
    const auto centre_y = static_cast<int>(exact_lround(pose.vanish_y))
        + static_cast<int>(std::trunc(pose.y * focal_length / pose.z));
    const auto left = centre_x - dimension / 2;
    const auto top = centre_y - dimension / 2;
    const auto source_width = static_cast<int>(texture.u_mask) + 1;
    const auto source_height = static_cast<int>(texture.v_mask) + 1;
    std::int32_t first_x = 0;
    std::int32_t last_x = dimension;
    if (pose.effect_clip_right > pose.effect_clip_left) {
        first_x = std::max<std::int32_t>(first_x, pose.effect_clip_left - left);
        last_x = std::min<std::int32_t>(last_x, pose.effect_clip_right - left);
    }
    if (first_x >= last_x) return;
    if(auto* commands=target.command_buffer()) {
        const int scale=int(target.draw_scale());RasterCommand command;
        command.left=(left+first_x)*scale;command.right=(left+last_x)*scale;
        command.top=top*scale;command.bottom=(top+dimension)*scale;
        command.textured=2;command.texture_offset=commands->texture(texture.texels);
        command.u_mask=texture.u_mask;command.v_mask=texture.v_mask;command.colour_base=colour_index_base;
        command.u=left*scale;command.v=top*scale;command.du=dimension;command.dv=scale;
        command.tag=target.layer_override()<0?1:target.layer_override();
        if(pose.palette_override) command.reserved0=256U+*pose.palette_override;
        commands->add(command);return;
    }
    for (auto y = 0; y < dimension; ++y) {
        const auto source_y = std::min(source_height - 1,
            static_cast<int>(static_cast<std::int64_t>(y) * source_height / dimension));
        for (auto x = first_x; x < last_x; ++x) {
            const auto source_x = std::min(source_width - 1,
                static_cast<int>(static_cast<std::int64_t>(x) * source_width / dimension));
            const auto texel = texture.texels[
                static_cast<std::size_t>(source_y) * source_width
                + static_cast<std::size_t>(source_x)];
            if (texel != 0U) {
                target.set(left + x, top + y,
                    pose.palette_override.value_or(
                        static_cast<std::uint8_t>(colour_index_base + texel)));
            }
        }
    }
}


} // namespace

SoftwareRenderer::SoftwareRenderer(RenderSettings settings) : settings_(settings) {}

void SoftwareRenderer::draw_cockpit_hud(
    const simulation::TrigTables& trigonometry,
    double rotation,
    std::uint8_t colour,
    std::uint8_t damage_flags,
    std::int32_t horizontal_origin,
    Framebuffer& target,
    std::uint8_t normal_colour_override) const {
    // Cockpit lines are plotted on the source raster rather than scan
    // converted, so they carry the same authored resolution as cartridge art
    // and read as blocky next to the filtered HUD around them. Put them in the
    // 2D layer so the filter resolves their diagonals too.
    const ScopedLayer layer{target, PixelLayer::two_d};
    struct HudPoint {
        std::int32_t x{};
        std::int32_t y{};
    };
    rotation = std::fmod(rotation, 256.0);
    if (rotation < 0.0) rotation += 256.0;
    const auto angle = static_cast<std::uint8_t>(rotation);
    const auto next_angle = static_cast<std::uint8_t>(angle + 1U);
    const auto fraction = rotation - angle;
    const auto sine = std::lerp(static_cast<double>(trigonometry.sin8(angle)),
        static_cast<double>(trigonometry.sin8(next_angle)), fraction);
    const auto cosine = std::lerp(static_cast<double>(trigonometry.cos8(angle)),
        static_cast<double>(trigonometry.cos8(next_angle)), fraction);
    const auto scale = static_cast<std::int32_t>(target.draw_scale());
    const StoredRasterScope scale_guard{target, static_cast<std::uint32_t>(scale)};
    const auto rotate = [sine, cosine, scale](std::int32_t x, std::int32_t y) {
        // MROTPNTY loads a signed eight-bit table entry into the high byte of
        // a long product and performs one final ASR: (value * trig) / 512.
        return HudPoint{
            static_cast<std::int32_t>(std::floor((x * cosine + y * sine) * scale / 512.0)),
            static_cast<std::int32_t>(std::floor((y * cosine - x * sine) * scale / 512.0)),
        };
    };
    const auto source_left = horizontal_origin * scale;
    const auto source_right = (horizontal_origin + 224) * scale;
    const auto vanish_x = (horizontal_origin + 112) * scale;
    const auto vanish_y = 96 * scale;
    const auto paint_line = [&target, source_left, source_right,
                             vanish_x, vanish_y, scale](
                                HudPoint a, HudPoint b,
                                std::uint8_t pixel) {
        auto x0 = a.x + vanish_x;
        auto y0 = a.y + vanish_y;
        const auto x1 = b.x + vanish_x;
        const auto y1 = b.y + vanish_y;
        const auto dx = std::abs(x1 - x0);
        const auto sx = x0 < x1 ? 1 : -1;
        const auto dy = std::abs(y1 - y0);
        const auto sy = y0 < y1 ? 1 : -1;
        const auto plot = [&] {
            // Keep the source line weight while allowing sub-source-pixel
            // motion at Render Upscale > 1; never round roll back to 8 bits.
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    if (x0 + dx >= source_left && x0 + dx < source_right)
                        target.set(x0 + dx, y0 + dy, pixel);
                }
            }
        };
        if (dx >= dy) {
            auto error = dx >> 1;
            for (auto count = dx + 1; count != 0; --count) {
                plot();
                error -= dy;
                if (error < 0) {
                    error += dx;
                    y0 += sy;
                }
                x0 += sx;
            }
        } else {
            auto error = dy >> 1;
            for (auto count = dy + 1; count != 0; --count) {
                plot();
                error -= dx;
                if (error < 0) {
                    error += dy;
                    x0 += sx;
                }
                y0 += sy;
            }
        }
    };
    const auto normal = normal_colour_override != 0U
        ? normal_colour_override
        : static_cast<std::uint8_t>(
            settings_.colour_index_base + (colour & 0x0fU));
    const auto damaged = static_cast<std::uint8_t>(
        settings_.colour_index_base + 2U);
    const auto positive_colour = (damage_flags & 2U) != 0U ? damaged : normal;
    const auto negative_colour = (damage_flags & 1U) != 0U ? damaged : normal;
    const auto line = [&](std::int32_t x1, std::int32_t y1,
                          std::int32_t x2, std::int32_t y2) {
        const auto a = rotate(x1, y1);
        const auto b = rotate(x2, y2);
        paint_line(a, b, positive_colour);
        paint_line({-a.x, -a.y}, {-b.x, -b.y}, negative_colour);
    };

    // Exact MHUD.MC source geometry: the vertical pair plus the mirrored
    // three-line wing indicators.
    line(0, 200, 0, 265);
    line(250, 0, 315, -32);
    line(315, -32, 315, 32);
    line(250, 0, 315, 32);
}

void apply_source_depth_tables(
    const assets::RomImage& rom,
    std::uint32_t depth_table_address,
    std::uint16_t threshold_pointer,
    std::uint16_t colour_pointer,
    std::uint8_t object_depth_offset,
    RenderPose& pose) {
    const auto data_bank = depth_table_address & 0xff0000U;
    if (data_bank >= 0x7e0000U
        || threshold_pointer < 0x8000U || colour_pointer < 0x8000U) {
        pose.has_depth_colour_tables = false;
        return;
    }
    if (object_depth_offset != 0U) {
        // MOBJ replaces the scene pointer with DEPTHTABLES + (offset-1)*4.
        // It does not index relative to the current scene's table. SCRAMBLE
        // uses offset 1 to keep its hangar bright despite tunnel depth cues.
        threshold_pointer = static_cast<std::uint16_t>(depth_table_address
            + static_cast<std::uint16_t>(object_depth_offset - 1U) * 4U);
    }
    for (std::size_t index = 0; index < pose.depth_thresholds.size(); ++index) {
        const auto encoded = std::bit_cast<std::int8_t>(
            rom.read8(data_bank | static_cast<std::uint16_t>(
                threshold_pointer + static_cast<std::uint16_t>(index))));
        pose.depth_thresholds[index] = static_cast<std::int16_t>(
            -static_cast<std::int16_t>(encoded) * 256);
    }
    for (std::size_t depth = 0; depth < pose.depth_colour_tables.size(); ++depth) {
        for (std::size_t colour = 0;
             colour < pose.depth_colour_tables[depth].size(); ++colour) {
            pose.depth_colour_tables[depth][colour] = rom.read8(
                data_bank | static_cast<std::uint16_t>(colour_pointer
                    + static_cast<std::uint16_t>(depth * 32U + colour)));
        }
    }
    pose.has_depth_colour_tables = true;
}

void SoftwareRenderer::draw(
    const assets::Shape& shape,const RenderPose& pose,Framebuffer& target,bool clear_target,
    SurfaceBuffer* surfaces,
    RenderDiagnostics* diagnostics,
    software_renderer_detail::Scratch* reusable_scratch) const {
    draw_impl(shape,pose,target,clear_target,surfaces,diagnostics,reusable_scratch);
}
void SoftwareRenderer::draw_impl(
    const assets::Shape& shape,
    const RenderPose& pose,
    Framebuffer& target,
    bool clear_target,
    SurfaceBuffer* surfaces,
    RenderDiagnostics* axis_diagnostics,
    software_renderer_detail::Scratch* reusable_scratch) const {
    if (axis_diagnostics) *axis_diagnostics = {};
    // Everything this renderer emits is the Super FX layer, whatever draw
    // scale each path happens to use. Scan conversion drops the scale to 1 and
    // would derive that correctly on its own, but the sprite paths below
    // (simple_scaled_sprite shapes, and sprite faces inside the face loop)
    // deliberately stay on the source raster, so the derivation would read
    // them as cartridge art and hand them to the 2D filter. Declaring the
    // layer once at the entry point makes the classification independent of
    // which path a shape takes.
    const ScopedLayer layer{target, PixelLayer::three_d};
    if (clear_target) {
        target.clear(settings_.background_colour);
        if (surfaces != nullptr) surfaces->clear();
    }
    // Interpolated presentation frames already keep fractional transformed
    // vertices. At a completed 20 Hz source frame the source-exact path
    // quantizes them again; that single-frame snap is hidden by the native
    // raster but becomes visible at higher presentation rates, even at 1x.
    // Use one continuous fractional copy for high-FPS/upscaled scan conversion,
    // visibility and BSP ordering. Mixing its edges with the rounded source
    // copy would make whole faces alternate between present and absent on
    // each completed 20 Hz frame.
    const auto continuous_geometry = settings_.render_scale > 1U
        || pose.continuous_geometry;
    // The raster pose differs from the caller's in this one flag only; the
    // pose itself is not copied for every model.
    const bool raster_subpixel = pose.subpixel_projection || continuous_geometry;
    const auto raster_word_exact = pose.use_rotation_matrix
        && !raster_subpixel;
    if (pose.simple_scaled_sprite) {
        const auto* texture = texture_for_colour(
            shape, pose.simple_sprite_colour, pose.colour_frame);
        if (texture != nullptr) {
            // Whole-object sprites (asteroids, explosion billboards) are
            // authored texels point-sampled onto the source raster, not
            // rasterized geometry. They are cartridge art wearing a shape's
            // clothes, so hand them to the 2D layer where a presentation
            // filter can resolve them.
            const ScopedLayer sprite_layer{target, PixelLayer::two_d};
            draw_simple_scaled_sprite(target, *texture, pose,
                settings_.focal_length, settings_.colour_index_base);
        }
        return;
    }
    software_renderer_detail::Scratch local_scratch;
    auto& scratch = reusable_scratch != nullptr
        ? *reusable_scratch : local_scratch;
    auto& transformed_vertices = scratch.transformed_vertices;
    auto& projected = scratch.projected;
    auto& ordered_faces = scratch.ordered_faces;
    auto& camera_polygon = scratch.camera_polygon;
    auto& camera_clipping_scratch = scratch.camera_clipping_scratch;
    auto& polygon = scratch.polygon;
    auto& raster_polygon = scratch.raster_polygon;
    auto& clipping_scratch = scratch.clipping_scratch;
    auto& word_projected = scratch.word_projected;
    auto& word_polygon = scratch.word_polygon;
    auto& word_clipping_scratch = scratch.word_clipping_scratch;
    transformed_vertices.clear();
    projected.clear();
    ordered_faces.clear();
    camera_polygon.clear();
    camera_clipping_scratch.clear();
    polygon.clear();
    raster_polygon.clear();
    clipping_scratch.clear();
    const auto frame_index = shape.frames.empty()
        ? std::size_t{0U}
        : static_cast<std::size_t>(pose.animation_frame)
            % shape.frames.size();
    const auto& vertices = shape.frames.empty()
        ? shape.vertices : shape.frames[frame_index].vertices;
    const auto shading = source_shading(pose);
    const auto depth_band = shading.depth_band;
    const auto& light = shading.light;

    const auto& word_coordinates = shape.frames.empty()
        ? shape.word_coordinates : shape.frames[frame_index].word_coordinates;
    const auto shifted_factor = shape.header.shift < 32U
        ? pose.scale * static_cast<double>(
            std::uint32_t{1} << shape.header.shift)
        : 0.0;
    // Cartridge geometry is signed Q15 with 16-bit wrapping, not FP64.
    // Prepare the pose once, then transform all eligible vertices using native
    // integer arithmetic. Fractional/continuous geometry keeps its reference.
    // In [0, 65536] the conversion to an integer is the truncation itself.
    const bool integer_transform = pose.use_rotation_matrix
        && !raster_subpixel && shifted_factor >= 0.0
        && shifted_factor <= 65536.0
        && static_cast<double>(static_cast<std::int32_t>(shifted_factor)) == shifted_factor;
    // At most 65536 here: a 32-bit conversion, not the library's 64-bit one.
    const std::int64_t integer_scale=integer_transform ? std::int32_t(shifted_factor) : 0;
    const std::array<std::int16_t,3> translation=integer_transform
        ? std::array{rounded_word(pose.x),rounded_word(pose.y),rounded_word(pose.z)}
        : std::array<std::int16_t,3>{};
    const bool word_projection = integer_transform && settings_.focal_length == 256.0
        && pose.stereo_eye_offset == 0;
    const auto vanish_x = word_projection ? rounded_word(pose.vanish_x) : std::int16_t{};
    const auto vanish_y = word_projection ? rounded_word(pose.vanish_y) : std::int16_t{};
    // Cartridge face indices are bytes. Keep the common geometry in signed
    // words, expanding only the uncommon textured/near-plane face that needs
    // the general clipper. Axis outputs retain their complete doubles.
    std::array<WordProjection, 256> word_transformed;
    const bool compact_geometry = word_projection && !pose.collapse_to_axis_line
        && vertices.size() <= word_transformed.size();
    if (!compact_geometry) {
        transformed_vertices.reserve(vertices.size());
        projected.reserve(vertices.size());
    }
    word_projected.clear();
    if (word_projection) word_projected.reserve(vertices.size());
    if (compact_geometry) {
        // The transform and projection of the loop below for the case every
        // cartridge model takes, with the matrix in registers and the output
        // written in place. Only the low word of point * scale survives
        // wrap16, and a 32-bit product has the same low word; the wrapping
        // 16-bit additions of the three shifted products and the translation
        // equal one wrap of their 32-bit sum.
        const auto count = vertices.size();
        word_projected.resize(count);
        auto* const projected_words = word_projected.data();
        const auto& matrix = pose.rotation_matrix;
        const std::int32_t m0 = matrix[0], m1 = matrix[1], m2 = matrix[2];
        const std::int32_t m3 = matrix[3], m4 = matrix[4], m5 = matrix[5];
        const std::int32_t m6 = matrix[6], m7 = matrix[7], m8 = matrix[8];
        const std::int32_t t0 = translation[0], t1 = translation[1], t2 = translation[2];
        const auto scale = static_cast<std::uint32_t>(integer_scale);
        const auto word_count = word_coordinates.size();
        for (std::size_t index = 0; index < count; ++index) {
            const auto& point = vertices[index];
            const auto factor = index < word_count && word_coordinates[index] ? 1U : scale;
            const std::int32_t x = static_cast<std::int16_t>(static_cast<std::uint16_t>(
                static_cast<std::uint32_t>(point.x) * factor));
            const std::int32_t y = static_cast<std::int16_t>(static_cast<std::uint16_t>(
                static_cast<std::uint32_t>(point.y) * factor));
            const std::int32_t z = static_cast<std::int16_t>(static_cast<std::uint16_t>(
                static_cast<std::uint32_t>(point.z) * factor));
            const auto camera_x = wrap16(((x * m0) >> 15) + ((y * m3) >> 15) + ((z * m6) >> 15) + t0);
            const auto camera_y = wrap16(((x * m1) >> 15) + ((y * m4) >> 15) + ((z * m7) >> 15) + t1);
            const auto camera_z = wrap16(((x * m2) >> 15) + ((y * m5) >> 15) + ((z * m8) >> 15) + t2);
            word_transformed[index] = {camera_x, camera_y, camera_z};
            projected_words[index] = project_word(camera_x, camera_y, camera_z, vanish_x, vanish_y);
        }
    } else
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        const auto& point = vertices[index];
        const auto word = index < word_coordinates.size() && word_coordinates[index];
        const auto factor = word ? 1.0 : shifted_factor;
        // Rasterization, visibility and BSP ordering all read this one
        // selected copy; no separate source-exact copy is transformed.
        Vec3 transformed;
        if (integer_transform) {
            const auto scale=word ? std::int64_t{1} : integer_scale;
            const std::array<std::int16_t,3> input{
                wrap16(std::int64_t(point.x)*scale),
                wrap16(std::int64_t(point.y)*scale),
                wrap16(std::int64_t(point.z)*scale)};
            std::array<std::int16_t,3> output{};
            for(unsigned col=0;col<3;++col) {
                auto value=multiply_q15(input[0],pose.rotation_matrix[col]);
                value=add16(value,multiply_q15(input[1],pose.rotation_matrix[3+col]));
                value=add16(value,multiply_q15(input[2],pose.rotation_matrix[6+col]));
                output[col]=add16(value,translation[col]);
            }
            if (compact_geometry)
                word_transformed[index] = {output[0], output[1], output[2]};
            else transformed={double(output[0]),double(output[1]),double(output[2])};
            if (word_projection)
                word_projected.push_back(project_word(output[0], output[1], output[2], vanish_x, vanish_y));
        } else transformed = rotate_with_factor(point, pose, factor, raster_subpixel);
        if (compact_geometry) continue;
        transformed_vertices.push_back(transformed);
        if (word_projection) {
            const auto& p = word_projected.back();
            projected.push_back({double(p.x), double(p.y), double(p.z), p.z >= 0});
        } else projected.push_back(project_point(
            transformed, settings_.focal_length,
            raster_word_exact, pose.vanish_x, pose.vanish_y,
            raster_subpixel, pose.stereo_eye_offset));
    }
    const auto& raster_vertices = transformed_vertices;
    const auto& visibility_vertices = raster_vertices;
    const auto& visibility_projected = projected;
    const auto projected_count = word_projection ? word_projected.size() : projected.size();
    const auto transformed_at = [&](std::size_t index) -> Vec3 {
        if (!compact_geometry) return raster_vertices[index];
        const auto& p = word_transformed[index];
        return {double(p.x), double(p.y), double(p.z)};
    };
    const auto projected_at = [&](std::size_t index) -> ScreenPoint {
        if (!compact_geometry) return projected[index];
        const auto& p = word_projected[index];
        return {double(p.x), double(p.y), double(p.z), p.z >= 0};
    };
    const auto face_visible = [&](const assets::Visibility& visibility) {
        if (word_projection) return word_visibility(visibility, word_projected);
        return continuous_geometry
            ? continuous_visibility(visibility, visibility_vertices)
            : source_visibility(
                visibility, visibility_projected, visibility_vertices);
    };

    // MOBJ.MC enters the face pass with r8 holding the end of M_PROJPNTS,
    // then COLOR WARP's mrand macro uses that register directly instead of
    // reading the colour table.  Its 16-bit SWAP/ROR/ADD/ADC/INC sequence is
    // the same one used by MGDOTS.MC.  Animated descriptors loop back through
    // .getwordagain; with warp enabled that consumes another random word.
    auto colour_warp_state = static_cast<std::uint16_t>(
        pose.projected_points_address
        + static_cast<std::uint16_t>(vertices.size() * 6U));
    auto colour_warp_carry = false;
    const auto next_colour_warp_word = [&]()
            -> std::optional<std::uint16_t> {
        if (!pose.colour_warp || pose.force_colour) return std::nullopt;
        auto word = std::uint16_t{};
        for (auto animation_hops = 0; animation_hops < 32; ++animation_hops) {
            const auto swapped = static_cast<std::uint16_t>(
                (colour_warp_state << 8U) | (colour_warp_state >> 8U));
            const auto rotated = static_cast<std::uint16_t>(
                (colour_warp_carry ? 0x8000U : 0U) | (swapped >> 1U));
            colour_warp_carry = (swapped & 1U) != 0U;
            const auto first = static_cast<std::uint32_t>(rotated)
                + colour_warp_state;
            colour_warp_carry = first > 0xffffU;
            const auto second = static_cast<std::uint32_t>(
                static_cast<std::uint16_t>(first)) + colour_warp_state
                + (colour_warp_carry ? 1U : 0U);
            colour_warp_carry = second > 0xffffU;
            colour_warp_state = static_cast<std::uint16_t>(second + 1U);
            word = colour_warp_state;
            if ((word & 0xc000U) != 0x8000U) return word;
        }
        // Corrupt/random data can theoretically select animated material
        // words forever. The hardware has no guard, but bounding a malformed
        // host frame keeps the option from hanging the process.
        return static_cast<std::uint16_t>(word & ~0x8000U);
    };

    if (pose.collapse_to_axis_line && !vertices.empty() && !shape.faces.empty()) {
        const auto [minimum, maximum] = std::minmax_element(
            vertices.begin(), vertices.end(), [](const auto& left, const auto& right) {
                return left.z < right.z;
            });
        Vec3 near_axis{};
        Vec3 far_axis{};
        std::size_t near_count{};
        std::size_t far_count{};
        for (std::size_t index = 0; index < vertices.size(); ++index) {
            if (vertices[index].z == maximum->z) {
                near_axis.x += raster_vertices[index].x;
                near_axis.y += raster_vertices[index].y;
                near_axis.z += raster_vertices[index].z;
                ++near_count;
            }
            if (vertices[index].z == minimum->z) {
                far_axis.x += raster_vertices[index].x;
                far_axis.y += raster_vertices[index].y;
                far_axis.z += raster_vertices[index].z;
                ++far_count;
            }
        }
        if (near_count != 0U && far_count != 0U) {
            near_axis.x /= static_cast<double>(near_count);
            near_axis.y /= static_cast<double>(near_count);
            near_axis.z /= static_cast<double>(near_count);
            far_axis.x /= static_cast<double>(far_count);
            far_axis.y /= static_cast<double>(far_count);
            far_axis.z /= static_cast<double>(far_count);
            if (axis_diagnostics) axis_diagnostics->camera = {{{near_axis.x,near_axis.y,near_axis.z},{far_axis.x,far_axis.y,far_axis.z}}};
            if (clip_near_line(near_axis, far_axis, raster_word_exact)) {
                auto near_screen = project_point(near_axis, settings_.focal_length,
                    raster_word_exact, pose.vanish_x,
                    pose.vanish_y, raster_subpixel, pose.stereo_eye_offset);
                auto far_screen = project_point(far_axis, settings_.focal_length,
                    raster_word_exact, pose.vanish_x,
                    pose.vanish_y, raster_subpixel, pose.stereo_eye_offset);
                if (axis_diagnostics) axis_diagnostics->projected = {{{near_screen.x,near_screen.y},{far_screen.x,far_screen.y}}};
                if (clip_screen_line(near_screen, far_screen, target,
                        raster_word_exact)) {
                    if (axis_diagnostics) {
                        axis_diagnostics->clipped = {{{near_screen.x,near_screen.y},{far_screen.x,far_screen.y}}};
                        axis_diagnostics->visible = true;
                    }
                    const auto material = face_material(shape, shape.faces.front(),
                        pose.colour_frame, depth_band, light, pose,
                        next_colour_warp_word(), settings_.colour_index_base);
                    const StoredRasterScope raster{
                        target, settings_.render_scale};
                    scale_to_stored(near_screen, settings_.render_scale);
                    scale_to_stored(far_screen, settings_.render_scale);
                    draw_line(target, near_screen, far_screen, material.colour,
                        settings_.colour_index_base, settings_.render_scale, settings_.wireframe_thickness);
                }
            }
        }
        return;
    }

    ordered_faces.reserve(shape.faces.size());
    if (shape.bsp_root_address == 0 || pose.explosion_progress != 0U) {
        for (const auto& face : shape.faces) {
            ordered_faces.push_back(&face);
        }
    } else {
        const auto append_batch = [&ordered_faces, &shape](std::uint32_t address) {
            const auto* batch = find_bsp_record(shape.face_batches,
                shape.bsp_batch_lookup, address);
            if (batch == nullptr) return;
            for (const auto& face : batch->faces) {
                ordered_faces.push_back(&face);
            }
        };
        // BSP traversal only needs the active recursion path for cycle
        // detection. Keep the common path inline so each model draw does not
        // allocate an unordered_set and a type-erased recursive callback.
        constexpr std::size_t kInlineBspDepth = 64U;
        std::array<std::uint32_t, kInlineBspDepth> active_path{};
        std::vector<std::uint32_t> active_path_overflow;
        auto active_path_size = std::size_t{};
        const auto is_active = [&](std::uint32_t address) {
            const auto inline_size = std::min(active_path_size,
                active_path.size());
            for (std::size_t index = 0; index < inline_size; ++index) {
                if (active_path[index] == address) return true;
            }
            return std::find(active_path_overflow.begin(),
                active_path_overflow.end(), address)
                != active_path_overflow.end();
        };
        const auto push_active = [&](std::uint32_t address) {
            if (active_path_size < active_path.size()) {
                active_path[active_path_size] = address;
            } else {
                active_path_overflow.push_back(address);
            }
            ++active_path_size;
        };
        const auto pop_active = [&] {
            --active_path_size;
            if (active_path_size >= active_path.size()) {
                active_path_overflow.pop_back();
            }
        };
        const auto traverse = [&](auto&& self, std::uint32_t address) -> void {
            if (address == 0 || is_active(address)) return;
            push_active(address);
            const auto* leaf = find_bsp_record(shape.bsp_leaves,
                shape.bsp_leaf_lookup, address);
            if (leaf != nullptr) {
                append_batch(leaf->face_batch_address);
                pop_active();
                return;
            }
            const auto* node = find_bsp_record(shape.bsp_nodes,
                shape.bsp_node_lookup, address);
            if (node == nullptr) {
                pop_active();
                return;
            }

            bool plane_visible = false;
            if (node->visibility_index < shape.visibilities.size()) {
                const auto& visibility = shape.visibilities[node->visibility_index];
                if (visibility.a < projected_count
                    && visibility.b < projected_count
                    && visibility.c < projected_count) {
                    plane_visible = face_visible(visibility);
                }
            }

            if (plane_visible) {
                self(self, node->fallthrough_address);
                append_batch(node->face_batch_address);
                self(self, node->alternate_address);
            } else {
                self(self, node->alternate_address);
                self(self, node->fallthrough_address);
            }
            pop_active();
        };
        traverse(traverse, shape.bsp_root_address);
    }

    // The raster extent in source pixels: a division by the draw scale each
    // time it is asked. The word path below runs at scale 1 only, where the
    // extent cannot change under a StoredRasterScope.
    const auto raster_width = target.width();
    const auto raster_height = target.height();
    // Face vertices are rebuilt for every polygon, but their storage is
    // reused across the entire object. Explosions visit every face, so
    // per-face vectors would allocate for each one on every video frame.
    for (const auto* face_pointer : ordered_faces) {
        const auto& face = *face_pointer;
        if (pose.explosion_progress == 0U && face.visibility_index >= 0
            && static_cast<std::size_t>(face.visibility_index) < shape.visibilities.size()) {
            const auto& visibility = shape.visibilities[
                static_cast<std::size_t>(face.visibility_index)];
            if (visibility.a >= projected_count
                || visibility.b >= projected_count
                || visibility.c >= projected_count) {
                continue;
            }
            if (!face_visible(visibility)) {
                continue;
            }
        }

        const auto material = face_material(
            shape, face, pose.colour_frame, depth_band, light, pose,
            next_colour_warp_word(), settings_.colour_index_base);
        // The common cartridge solid face needs neither FP64 camera copies,
        // unused texture interpolation, nor four complete copies when already
        // inside the viewport. Near-plane intersections and special outputs
        // retain the reference below. The scan converter is shared verbatim.
        if (word_projection && !face.sprite && material.texture == nullptr
            && pose.explosion_progress == 0U && surfaces == nullptr
            && axis_diagnostics == nullptr && !settings_.backface_culling
            && settings_.render_scale == 1U && face.vertex_indices.size() >= 3U) {
            const auto corners = face.vertex_indices.size();
            word_polygon.reserve(corners + 2U);
            word_polygon.resize(corners);
            auto* corner = word_polygon.data();
            bool eligible = true;
            // A polygon wholly inside the raster passes all four clip planes
            // unchanged and the clamp leaves it alone: skip both.
            bool inside = true;
            for (const auto index : face.vertex_indices) {
                if (index >= word_projected.size() || word_projected[index].z < 0) {
                    eligible = false;
                    break;
                }
                const auto& p = word_projected[index];
                *corner++ = {p.x, p.y};
                inside = inside && static_cast<std::uint32_t>(p.x) < raster_width
                    && static_cast<std::uint32_t>(p.y) < raster_height;
            }
            if (eligible) {
                if (!inside) {
                    clip_word_polygon(word_polygon, word_clipping_scratch,
                        std::int16_t(raster_width), std::int16_t(raster_height));
                    for (auto& p : word_polygon) {
                        p.x = std::clamp<std::int32_t>(p.x, 0, raster_width);
                        p.y = std::clamp<std::int32_t>(p.y, 0, raster_height);
                    }
                }
                const StoredRasterScope raster{target, settings_.render_scale};
                fill_source_polygon_points(target, word_polygon, material.colour,
                    settings_.colour_index_base, pose, false, nullptr, {});
                continue;
            }
        }
        const auto face_offset = explosion_offset(face, pose);
        if (face.sprite) {
            if (material.texture != nullptr && face.vertex_indices.size() == 1U
                && face.vertex_indices[0] < projected_count
                && projected_at(face.vertex_indices[0]).visible) {
                auto centre = transformed_at(face.vertex_indices[0]);
                centre.x += face_offset.x;
                centre.y += face_offset.y;
                centre.z += face_offset.z;
                // Sprite faces are texel art too; see the simple_scaled_sprite
                // branch. The polygon faces around them stay geometry.
                const ScopedLayer sprite_layer{target, PixelLayer::two_d};
                draw_textured_sprite(target, project_point(centre,
                    settings_.focal_length, raster_word_exact,
                    pose.vanish_x, pose.vanish_y,
                    raster_subpixel, pose.stereo_eye_offset),
                    *material.texture, settings_.colour_index_base);
            }
            continue;
        }
        camera_polygon.clear();
        // Clipping can add one vertex at each transition, and this buffer
        // swaps with camera_clipping_scratch. Keep both reusable allocations
        // large enough so later frames do not grow the input buffer again.
        camera_polygon.reserve(face.vertex_indices.size() + 2U);
        bool valid = true;
        bool any_behind = false;
        for (const auto index : face.vertex_indices) {
            if (index >= vertices.size()) {
                valid = false;
                break;
            }
            auto point = transformed_at(index);
            point.x += face_offset.x;
            point.y += face_offset.y;
            point.z += face_offset.z;
            any_behind = any_behind || point.z < 0.0;
            camera_polygon.push_back(point);
        }
        if (!valid || camera_polygon.size() < 2) {
            continue;
        }

        auto surface = SurfaceSample{};
        if (surfaces != nullptr) {
            auto normal_pose = pose;
            normal_pose.x = 0.0;
            normal_pose.y = 0.0;
            normal_pose.z = 0.0;
            normal_pose.scale = 1.0;
            const auto transformed_normal = rotate(face.normal, normal_pose, 0U);
            const auto normal_length = std::sqrt(
                transformed_normal.x * transformed_normal.x
                + transformed_normal.y * transformed_normal.y
                + transformed_normal.z * transformed_normal.z);
            if (normal_length > 0.0001) {
                surface.normal_x = static_cast<float>(
                    transformed_normal.x / normal_length);
                surface.normal_y = static_cast<float>(
                    transformed_normal.y / normal_length);
                surface.normal_z = static_cast<float>(
                    transformed_normal.z / normal_length);
            }
            surface.depth = static_cast<float>(std::accumulate(
                camera_polygon.begin(), camera_polygon.end(), 0.0,
                [](double total, const Vec3& point) { return total + point.z; })
                / static_cast<double>(camera_polygon.size()));
        }

        if (any_behind && material.texture != nullptr) {
            // MOBJ.MC explicitly declines to 3D-clip texture maps.
            continue;
        }

        if (camera_polygon.size() == 2U) {
            if (!clip_near_line(
                    camera_polygon[0], camera_polygon[1],
                    raster_word_exact)) {
                continue;
            }
        } else if (any_behind) {
            clip_near_polygon(camera_polygon, raster_word_exact,
                camera_clipping_scratch);
            camera_polygon.swap(camera_clipping_scratch);
            if (camera_polygon.size() < 3U) continue;
        }

        polygon.clear();
        polygon.reserve(camera_polygon.size());
        if (!any_behind && pose.explosion_progress == 0U) {
            // These are the unchanged vertices already projected for source
            // visibility, so shared mesh vertices do not repeat the same
            // divides/word rounding for every visible face. Reuse only while
            // near clipping and per-face explosion offsets cannot have
            // changed the camera polygon (including its vertex count/order).
            for (const auto index : face.vertex_indices)
                polygon.push_back(projected_at(index));
        } else {
            for (const auto& point : camera_polygon) {
                polygon.push_back(project_point(
                    point, settings_.focal_length,
                    raster_word_exact, pose.vanish_x,
                    pose.vanish_y, raster_subpixel,
                    pose.stereo_eye_offset));
            }
        }

        if (polygon.size() == 2) {
            if (!clip_screen_line(
                    polygon[0], polygon[1], target,
                    raster_word_exact)) {
                continue;
            }
            const StoredRasterScope raster{target, settings_.render_scale};
            scale_to_stored(polygon[0], settings_.render_scale);
            scale_to_stored(polygon[1], settings_.render_scale);
            draw_line(target, polygon[0], polygon[1], material.colour,
                settings_.colour_index_base, settings_.render_scale, settings_.wireframe_thickness);
            continue;
        }

        double signed_area = 0.0;
        if (settings_.backface_culling || axis_diagnostics)
        for (std::size_t index = 0; index < polygon.size(); ++index) {
            const auto& a = polygon[index];
            const auto next_index = index + 1U == polygon.size()
                ? std::size_t{0U} : index + 1U;
            const auto& b = polygon[next_index];
            signed_area += a.x * b.y - b.x * a.y;
        }
        if (axis_diagnostics) {
            PolygonRenderDiagnostics trace;
            for (const auto& point : camera_polygon) trace.camera.push_back({point.x,point.y,point.z});
            for (const auto& point : polygon) trace.projected.push_back({point.x,point.y});
            trace.signed_area=signed_area;
            axis_diagnostics->polygons.push_back(std::move(trace));
        }
        if (settings_.backface_culling && !face.sprite && signed_area >= 0.0) {
            continue;
        }
        raster_polygon.clear();
        raster_polygon.reserve(polygon.size());
        auto texture_coordinate_index = std::size_t{0U};
        for (std::size_t index = 0; index < polygon.size(); ++index) {
            auto texture = TexturePoint{};
            if (material.texture != nullptr) {
                const auto& coordinates = material.texture->coordinates;
                const auto& coordinate = coordinates[texture_coordinate_index];
                texture = {static_cast<double>(coordinate.u),
                    static_cast<double>(coordinate.v)};
                if (++texture_coordinate_index == coordinates.size())
                    texture_coordinate_index = 0U;
            }
            raster_polygon.push_back({polygon[index], texture});
        }
        raster_polygon = clip_screen_polygon(
            std::move(raster_polygon), target, raster_word_exact,
            clipping_scratch);
        if (raster_polygon.size() < 3U) continue;
        const StoredRasterScope raster{target, settings_.render_scale};
        for (auto& vertex : raster_polygon) {
            scale_to_stored(vertex.point, settings_.render_scale);
        }
        if (material.texture == nullptr) {
            fill_source_polygon(target, raster_polygon, material.colour,
                settings_.colour_index_base, pose,
                settings_.render_scale > 1U, surfaces, surface);
        } else {
            fill_source_textured_polygon(target, raster_polygon,
                *material.texture, pose.texture_scroll_x, pose.texture_scroll_y,
                settings_.colour_index_base, settings_.render_scale > 1U,
                surfaces, surface);
        }
    }
}

} // namespace starfox::render
