#pragma once

#include "starfox/assets/rom.hpp"

#include <array>
#include <cstdint>

// Fixed-point math of the simulation, also used by the renderers and the 3DS
// frontend: 16-bit words that wrap as the cartridge's do, its sine tables, and
// Q15 3x3 matrices. starfox/render/word_math.hpp holds inline copies of the
// word primitives; tests/word_math_test.cpp checks that both give equal results.

namespace starfox::simulation {

// The low 16 bits of `value`, read as a signed word.
[[nodiscard]] std::int16_t wrap16(std::int64_t value) noexcept;
[[nodiscard]] std::int16_t add16(std::int16_t left, std::int16_t right) noexcept;
[[nodiscard]] std::int16_t subtract16(std::int16_t left, std::int16_t right) noexcept;
// value / 2^bits rounded toward minus infinity, which is a signed shift right;
// 31 or more bits give 0 or -1.
[[nodiscard]] std::int32_t arithmetic_shift_right(std::int32_t value, unsigned bits) noexcept;
// (left * right) >> 15, rounded toward minus infinity, wrapped to 16 bits.
[[nodiscard]] std::int16_t multiply_q15(std::int16_t left, std::int16_t right) noexcept;

// The cartridge's sine tables, read from ROM: SINTAB and COSTAB (256 signed
// bytes) and SINTAB16 (256 Q15 words), one entry per 1/256 turn.
class TrigTables {
public:
    // Throws std::runtime_error when a table symbol is missing.
    [[nodiscard]] static TrigTables load(
        const assets::RomImage& rom, const assets::SymbolMap& symbols);

    // Angles in 1/256 turns.
    [[nodiscard]] std::int8_t sin8(std::uint8_t angle) const noexcept;
    [[nodiscard]] std::int8_t cos8(std::uint8_t angle) const noexcept;
    // Angles in 1/65536 turns. sin_q15 interpolates between SINTAB16 entries
    // with the low byte, rounding down; cos_q15(a) is sin_q15(a + $4000).
    [[nodiscard]] std::int16_t sin_q15(std::uint16_t angle) const noexcept;
    [[nodiscard]] std::int16_t cos_q15(std::uint16_t angle) const noexcept;

private:
    std::array<std::int8_t, 256> sine8_{};
    std::array<std::int8_t, 256> cosine8_{};
    std::array<std::int16_t, 256> sine_q15_{};
};

// 3x3 matrix of Q15 coefficients, element (row, column) at row * 3 + column.
using MatrixQ15 = std::array<std::int16_t, 9>;

// Rotation matrix for three angles in 1/65536 turns (GameSimulation builds
// GETVIEW_L's world matrix with it): sin_q15/cos_q15 terms combined with
// multiply_q15 and wrapping sums.
[[nodiscard]] MatrixQ15 rotation_matrix_q15(
    const TrigTables& trig,
    std::int16_t x,
    std::int16_t y,
    std::int16_t z) noexcept;
[[nodiscard]] MatrixQ15 transpose_q15(const MatrixQ15& matrix) noexcept;
// left * right with every product through multiply_q15 and 16-bit wrapping
// sums, as the cartridge composes matrices.
[[nodiscard]] MatrixQ15 multiply_matrix_q15(
    const MatrixQ15& left, const MatrixQ15& right) noexcept;
// Q15 quaternion rounding can slightly exceed unit length. Compose those
// matrices with saturation, never the native arithmetic's 16-bit wraparound.
[[nodiscard]] MatrixQ15 multiply_presentation_matrix_q15(
    const MatrixQ15& left, const MatrixQ15& right) noexcept;

// Presentation-only normalized matrix interpolation. This is deliberately
// separate from source state: it smooths rotations between 20 Hz updates
// without feeding fractional transforms back into cartridge simulation.
// alpha <= 0 returns `previous`; alpha >= 1, or equal matrices, `current`.
// Otherwise it slerps the two rotations' quaternions along the shorter arc.
[[nodiscard]] MatrixQ15 interpolate_rotation_matrix_q15(
    const MatrixQ15& previous,
    const MatrixQ15& current,
    double alpha) noexcept;
// Row vector times matrix: result[c] is the wrapping sum over r of
// multiply_q15(value[r], matrix[r * 3 + c]).
[[nodiscard]] std::array<std::int16_t, 3> transform_q15(
    const MatrixQ15& matrix,
    const std::array<std::int16_t, 3>& value) noexcept;

} // namespace starfox::simulation
