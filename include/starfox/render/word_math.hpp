#pragma once

#include <array>
#include <cmath>
#include <cstdint>

// Inline twins of the 16-bit word primitives in src/simulation/math.cpp.
//
// The originals live in another translation unit and the 3DS build has no
// link-time optimization, so every call is a real branch-and-link. The world
// renderer runs them per vertex, per face and per scanline; on an ARM11 the
// call costs more than the arithmetic. These are the same operations, written
// so the compiler can fold them into the caller. tests/word_math_test.cpp
// checks each one against the original over its complete input domain.
namespace starfox::render::word {

// simulation::wrap16: the low 16 bits, read as a signed word.
[[nodiscard]] constexpr std::int16_t wrap16(std::int64_t value) noexcept {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(
        static_cast<std::uint64_t>(value)));
}

[[nodiscard]] constexpr std::int16_t add16(std::int16_t left, std::int16_t right) noexcept {
    return wrap16(static_cast<std::int32_t>(left) + right);
}

[[nodiscard]] constexpr std::int16_t subtract16(std::int16_t left, std::int16_t right) noexcept {
    return wrap16(static_cast<std::int32_t>(left) - right);
}

// simulation::arithmetic_shift_right rounds a negative value toward minus
// infinity: -((magnitude + 2^bits - 1) / 2^bits) is floor(value / 2^bits),
// which is exactly a signed right shift (arithmetic since C++20).
[[nodiscard]] constexpr std::int32_t arithmetic_shift_right(
    std::int32_t value, unsigned bits) noexcept {
    return bits >= 31U ? (value < 0 ? -1 : 0) : value >> bits;
}

[[nodiscard]] constexpr std::int16_t multiply_q15(std::int16_t left, std::int16_t right) noexcept {
    return wrap16((static_cast<std::int32_t>(left) * static_cast<std::int32_t>(right)) >> 15);
}

// simulation::transform_q15. Each product is shifted on its own, as the
// source does; the 16-bit additions wrap, so one final wrap of their 32-bit
// sum yields the same word.
[[nodiscard]] constexpr std::array<std::int16_t, 3> transform_q15(
    const std::array<std::int16_t, 9>& matrix,
    const std::array<std::int16_t, 3>& value) noexcept {
    const auto component = [&](unsigned column) {
        return wrap16(((static_cast<std::int32_t>(value[0]) * matrix[column]) >> 15)
            + ((static_cast<std::int32_t>(value[1]) * matrix[3U + column]) >> 15)
            + ((static_cast<std::int32_t>(value[2]) * matrix[6U + column]) >> 15));
    };
    return {component(0U), component(1U), component(2U)};
}

// simulation::multiply_matrix_q15, the cartridge's wrapping composition.
[[nodiscard]] constexpr std::array<std::int16_t, 9> multiply_matrix_q15(
    const std::array<std::int16_t, 9>& left,
    const std::array<std::int16_t, 9>& right) noexcept {
    std::array<std::int16_t, 9> result{};
    for (unsigned row = 0U; row < 3U; ++row)
        for (unsigned column = 0U; column < 3U; ++column)
            result[row * 3U + column] = wrap16(
                ((static_cast<std::int32_t>(left[row * 3U]) * right[column]) >> 15)
                + ((static_cast<std::int32_t>(left[row * 3U + 1U]) * right[3U + column]) >> 15)
                + ((static_cast<std::int32_t>(left[row * 3U + 2U]) * right[6U + column]) >> 15));
    return result;
}

// simulation::multiply_presentation_matrix_q15: the same products, summed
// without wrapping and saturated to a signed word.
[[nodiscard]] constexpr std::array<std::int16_t, 9> multiply_presentation_matrix_q15(
    const std::array<std::int16_t, 9>& left,
    const std::array<std::int16_t, 9>& right) noexcept {
    std::array<std::int16_t, 9> result{};
    for (unsigned row = 0U; row < 3U; ++row)
        for (unsigned column = 0U; column < 3U; ++column) {
            const std::int32_t value =
                ((static_cast<std::int32_t>(left[row * 3U]) * right[column]) >> 15)
                + ((static_cast<std::int32_t>(left[row * 3U + 1U]) * right[3U + column]) >> 15)
                + ((static_cast<std::int32_t>(left[row * 3U + 2U]) * right[6U + column]) >> 15);
            result[row * 3U + column] = static_cast<std::int16_t>(
                value < -32'768 ? -32'768 : value > 32'767 ? 32'767 : value);
        }
    return result;
}

// std::lround without the library call. For a magnitude below 2^31 the
// truncated value and the remainder are both exact, so "nearest, halves away
// from zero" is decided exactly as the C library defines it. Anything else
// (including NaN) still goes to the library, whatever it answers there.
[[nodiscard]] inline long exact_lround(double value) noexcept {
    if (!(value > -2147483000.0 && value < 2147483000.0)) return std::lround(value);
    const auto truncated = static_cast<std::int32_t>(value);
    const double remainder = value - static_cast<double>(truncated);
    if (remainder >= 0.5) return static_cast<long>(truncated) + 1;
    if (remainder <= -0.5) return static_cast<long>(truncated) - 1;
    return truncated;
}

} // namespace starfox::render::word
