#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace starfox::platform_3ds {

// Incremental SHA-256 for diagnostic-file integrity. No OS hashing service,
// allocation, or platform byte-order assumptions. finish() consumes the state.
class DiagnosticSha256 {
public:
    void update(std::span<const std::uint8_t> input) noexcept {
        input_bytes_ += static_cast<std::uint64_t>(input.size());
        if (buffered_ != 0U) {
            const auto count = std::min(input.size(), buffer_.size() - buffered_);
            if (count != 0U) std::memcpy(buffer_.data() + buffered_, input.data(), count);
            buffered_ += count;
            input = input.subspan(count);
            if (buffered_ == buffer_.size()) {
                transform(buffer_.data());
                buffered_ = 0U;
            }
        }
        while (input.size() >= buffer_.size()) {
            transform(input.data());
            input = input.subspan(buffer_.size());
        }
        if (!input.empty()) {
            std::memcpy(buffer_.data(), input.data(), input.size());
            buffered_ = input.size();
        }
    }

    [[nodiscard]] std::array<std::uint8_t, 32> finish() noexcept {
        const auto input_bits = input_bytes_ * 8U;
        buffer_[buffered_++] = 0x80U;
        if (buffered_ > 56U) {
            std::fill(buffer_.begin() + buffered_, buffer_.end(), 0U);
            transform(buffer_.data());
            buffered_ = 0U;
        }
        std::fill(buffer_.begin() + buffered_, buffer_.begin() + 56U, 0U);
        for (unsigned index = 0U; index < 8U; ++index)
            buffer_[63U - index] = static_cast<std::uint8_t>(input_bits >> (index * 8U));
        transform(buffer_.data());
        std::array<std::uint8_t, 32> result{};
        for (std::size_t index = 0U; index < state_.size(); ++index)
            for (unsigned byte = 0U; byte < 4U; ++byte)
                result[index * 4U + byte] = static_cast<std::uint8_t>(state_[index] >> (24U - byte * 8U));
        return result;
    }

private:
    static constexpr std::array<std::uint32_t, 64> round_constants_{
        0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
        0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
        0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
        0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
        0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
        0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
        0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
        0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U};

    void transform(const std::uint8_t* block) noexcept {
        std::array<std::uint32_t, 64> words;
        for (std::size_t index = 0U; index < 16U; ++index) {
            const auto* p = block + index * 4U;
            words[index] = (std::uint32_t{p[0]} << 24U) | (std::uint32_t{p[1]} << 16U)
                | (std::uint32_t{p[2]} << 8U) | p[3];
        }
        for (std::size_t index = 16U; index < words.size(); ++index) {
            const auto x = words[index - 15U], y = words[index - 2U];
            words[index] = words[index - 16U]
                + (std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3U))
                + words[index - 7U]
                + (std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10U));
        }
        auto a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        auto e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (std::size_t index = 0U; index < words.size(); ++index) {
            const auto first = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25))
                + ((e & f) ^ (~e & g)) + round_constants_[index] + words[index];
            const auto second = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22))
                + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + first;
            d = c; c = b; b = a; a = first + second;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,
        0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U};
    std::array<std::uint8_t, 64> buffer_{};
    std::uint64_t input_bytes_{};
    std::size_t buffered_{};
};

} // namespace starfox::platform_3ds
