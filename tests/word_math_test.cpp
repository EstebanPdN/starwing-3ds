// render::word (include/starfox/render/word_math.hpp) against the functions
// it inlines: src/simulation/math.cpp and the C library's lround.
//
//   word_math_test          sampled domains, a few seconds
//   word_math_test --full   every pair of 16-bit operands and every 32-bit
//                           shift operand (about a minute)
#include "starfox/render/word_math.hpp"
#include "starfox/simulation/math.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>

using namespace starfox;
namespace word = starfox::render::word;

namespace {
unsigned long failures = 0;
void expect(bool ok, const char* what, long long a = 0, long long b = 0) {
    if (ok) return;
    if (failures++ < 10) std::printf("MISMATCH %s (%lld, %lld)\n", what, a, b);
}
}

int main(int argc, char** argv) {
    const bool full = argc > 1 && std::strcmp(argv[1], "--full") == 0;
    const int stride_a = full ? 1 : 7, stride_b = full ? 1 : 11;
    unsigned long long pairs = 0;
    for (int a = -32768; a <= 32767; a += stride_a) {
        for (int b = -32768; b <= 32767; b += stride_b) {
            const auto x = static_cast<std::int16_t>(a), y = static_cast<std::int16_t>(b);
            expect(word::add16(x, y) == simulation::add16(x, y), "add16", a, b);
            expect(word::subtract16(x, y) == simulation::subtract16(x, y), "subtract16", a, b);
            expect(word::multiply_q15(x, y) == simulation::multiply_q15(x, y), "multiply_q15", a, b);
            ++pairs;
        }
    }
    // The edges of both operands, whatever the stride skipped.
    for (int a : {-32768, -32767, -1, 0, 1, 32766, 32767})
        for (int b = -32768; b <= 32767; ++b) {
            const auto x = static_cast<std::int16_t>(a), y = static_cast<std::int16_t>(b);
            expect(word::multiply_q15(x, y) == simulation::multiply_q15(x, y), "multiply_q15 edge", a, b);
            expect(word::multiply_q15(y, x) == simulation::multiply_q15(y, x), "multiply_q15 edge", b, a);
            expect(word::add16(x, y) == simulation::add16(x, y), "add16 edge", a, b);
            expect(word::subtract16(x, y) == simulation::subtract16(x, y), "subtract16 edge", a, b);
            expect(word::subtract16(y, x) == simulation::subtract16(y, x), "subtract16 edge", b, a);
        }

    unsigned long long shifts = 0;
    const std::uint32_t shift_stride = full ? 1U : 4099U;
    for (unsigned bits : {1U, 2U, 7U, 8U, 15U}) {
        std::uint32_t v = 0;
        do {
            const auto value = static_cast<std::int32_t>(v);
            expect(word::arithmetic_shift_right(value, bits)
                == simulation::arithmetic_shift_right(value, bits), "arithmetic_shift_right", value, bits);
            ++shifts;
            const auto next = v + shift_stride;
            if (next < v) break;
            v = next;
        } while (true);
    }
    std::mt19937_64 rng(20261114);
    for (unsigned long i = 0; i < 20'000'000UL; ++i) {
        const auto value = static_cast<std::int32_t>(rng());
        const auto bits = static_cast<unsigned>(rng() % 70U);
        expect(word::arithmetic_shift_right(value, bits)
            == simulation::arithmetic_shift_right(value, bits), "arithmetic_shift_right random", value, bits);
        const auto wide = static_cast<std::int64_t>(rng());
        expect(word::wrap16(wide) == simulation::wrap16(wide), "wrap16", wide);
        expect(word::wrap16(value) == simulation::wrap16(value), "wrap16 32", value);
    }
    for (std::int32_t value : {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max(), -1, 0, 1})
        for (unsigned bits = 0; bits < 80U; ++bits)
            expect(word::arithmetic_shift_right(value, bits)
                == simulation::arithmetic_shift_right(value, bits), "arithmetic_shift_right edge", value, bits);

    // Matrix forms: random words, plus saturating and wrapping extremes.
    for (unsigned long i = 0; i < 6'000'000UL; ++i) {
        simulation::MatrixQ15 a, b;
        std::array<std::int16_t, 3> v;
        const bool extreme = i % 5U == 0U;
        for (auto& m : a) m = static_cast<std::int16_t>(extreme ? (rng() % 2 ? 32767 : -32768) : rng());
        for (auto& m : b) m = static_cast<std::int16_t>(i % 7U == 0U ? (rng() % 2 ? 32767 : -32768) : rng());
        for (auto& m : v) m = static_cast<std::int16_t>(rng());
        expect(word::transform_q15(a, v) == simulation::transform_q15(a, v), "transform_q15", static_cast<long long>(i));
        expect(word::multiply_matrix_q15(a, b) == simulation::multiply_matrix_q15(a, b), "multiply_matrix_q15", static_cast<long long>(i));
        expect(word::multiply_presentation_matrix_q15(a, b)
            == simulation::multiply_presentation_matrix_q15(a, b), "multiply_presentation_matrix_q15", static_cast<long long>(i));
    }

    // exact_lround against lround: halves and their neighbours, random
    // values of every magnitude, and what lies outside the inlined range.
    unsigned long long rounds = 0;
    const auto check_round = [&](double value) {
        ++rounds;
        expect(word::exact_lround(value) == std::lround(value), "exact_lround",
            static_cast<long long>(value * 4.0));
    };
    for (int n = -140000; n <= 140000; ++n) {
        const double half = n * 0.5;
        check_round(half);
        check_round(std::nextafter(half, -1e300));
        check_round(std::nextafter(half, 1e300));
    }
    for (double value : {0.0, -0.0, 0.49999999999999994, -0.49999999999999994, 0.5, -0.5,
            2147482999.5, -2147482999.5, 2147483000.0, -2147483000.0, 2147483647.0, -2147483648.0,
            2147483647.5, 4294967296.5, -4294967296.5, 1e15, -1e15, 4503599627370496.5})
        check_round(value);
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    for (unsigned long i = 0; i < 40'000'000UL; ++i) {
        const double scale = std::ldexp(1.0, static_cast<int>(rng() % 36U) - 4);
        check_round(unit(rng) * scale);
        // The interpolation the renderer rounds: a word plus a fraction of a difference.
        const double from = static_cast<std::int16_t>(rng());
        const double delta = static_cast<double>(static_cast<std::int32_t>(rng() % 65536U) - 32768);
        check_round(from + delta * (static_cast<double>(rng() % 8U) / 7.0));
        check_round(from + delta * (static_cast<double>(rng() % 4U) / 3.0));
    }

    std::printf("word_math_test%s: pairs=%llu shifts=%llu rounds=%llu failures=%lu\n",
        full ? " --full" : "", pairs, shifts, rounds, failures);
    return failures != 0;
}
