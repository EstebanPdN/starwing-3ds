#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace starfox::simulation::detail {

// Marks left by stores into one block of work RAM (the object tables), so
// that whoever mirrors that block can tell what changed without comparing all
// of it. Every writer of the block must leave its mark or set `all`.
//
// The block is cut in 64-byte chunks. A store marks the chunk of its first
// byte only, so a mark on chunk N stands for N's 64 bytes and the one after
// them (a two-byte store at the end of N reaches it).
struct LeanWatch {
    static constexpr std::uint32_t chunk_bits = 6U;
    static constexpr std::uint32_t chunk_size = 1U << chunk_bits;
    static constexpr std::uint32_t kept_size = chunk_size + 1U;
    static constexpr std::uint32_t max_chunks = 192U;
    // The block is work RAM from $0300 to $2fff: the object tables and what
    // lies between them. The direct page and the stack, below it, are where
    // most stores go and are not watched, except for the very first byte of
    // work RAM, which the caller of the loop services.
    static constexpr std::uint32_t first = 0x0300U, last = 0x3000U;
    using Marks = std::array<std::uint8_t, max_chunks>;
    std::uint8_t* base{};       // host address of the block; null when nothing is watched
    std::uint32_t size{};       // its size, a multiple of the chunk size
    // Chunks the loop need not report again: stored to since the last
    // checkpoint and holding no serviced byte.
    Marks seen{};
    // Chunks stored to since the last checkpoint, and since the marks were
    // last cleared.
    Marks kept{}, dirty{};
    // The chunk that holds the second byte the caller of the loop services
    // (~0: none). A store into it is always reported.
    std::uint32_t trap{~std::uint32_t{0}};
    // A writer that leaves no marks ran: any byte may have changed.
    bool all{true};
    // While a checkpoint is open, the old bytes of each chunk (and the byte
    // after it) at its first store: `logged` lists the chunks in the order
    // they were stored to, `log` holds kept_size bytes for each.
    bool logging{};
    std::uint32_t logged_count{};
    Marks logged{};
    std::vector<std::uint8_t> log;
    // The whole block as it was at the checkpoint, once a writer that leaves
    // no marks has run inside one.
    bool imaged{};
    std::vector<std::uint8_t> image;

    // A store is about to land in this chunk and `seen` does not have it.
    // True when the chunk holds a serviced byte.
    bool first_store(std::uint32_t chunk) noexcept {
        if (kept[chunk] == 0U) {
            if (logging && logged_count < max_chunks) {
                if (log.size() < std::size_t{max_chunks} * kept_size)
                    log.resize(std::size_t{max_chunks} * kept_size);
                std::copy_n(base + (std::size_t{chunk} << chunk_bits), kept_size,
                    log.data() + std::size_t{logged_count} * kept_size);
                logged[logged_count++] = static_cast<std::uint8_t>(chunk);
            }
            kept[chunk] = 1U;
            dirty[chunk] = 1U;
        }
        if (chunk == trap) return true;
        seen[chunk] = 1U;
        return false;
    }
    // A store of `count` bytes at this host address, by any other writer.
    void store(const std::uint8_t* at, std::size_t count) noexcept {
        if (base == nullptr || count == 0U || at + count <= base || at >= base + size) return;
        const std::size_t from = at <= base ? 0U : static_cast<std::size_t>(at - base);
        const std::size_t to = std::min<std::size_t>(size, static_cast<std::size_t>(at + count - base)) - 1U;
        for (std::size_t chunk = from >> chunk_bits; chunk <= to >> chunk_bits; ++chunk)
            if (seen[chunk] == 0U) static_cast<void>(first_store(static_cast<std::uint32_t>(chunk)));
    }
    // The block as it was when the checkpoint opened, written over `output`
    // (which holds work RAM from `offset` on, as it is now or as it was).
    void rewind(std::uint32_t offset, std::span<std::uint8_t> output) const noexcept {
        for (std::uint32_t entry = logged_count; entry-- != 0U;) {
            const std::uint32_t at = first + (std::uint32_t{logged[entry]} << chunk_bits);
            const std::uint32_t from = std::max(at, offset);
            const std::uint32_t to = std::min<std::uint32_t>(at + kept_size,
                offset + static_cast<std::uint32_t>(output.size()));
            if (from < to)
                std::copy_n(log.data() + std::size_t{entry} * kept_size + (from - at), to - from,
                    output.data() + (from - offset));
        }
    }
    // A writer that leaves no marks is about to run.
    void untracked() {
        if (logging) {
            // What the log says so far, taken to a full copy while it still can be.
            image.assign(base, base + size + 1U);
            rewind(first, image);
            imaged = true;
            logging = false;
        }
        all = true;
    }
    // From here on the first store to each chunk keeps its old bytes.
    void open_checkpoint() noexcept {
        seen = {}; kept = {};
        logged_count = 0U;
        imaged = false;
        logging = base != nullptr;
    }
    void close_checkpoint() noexcept {
        logging = false;
        imaged = false;
        seen = {};
    }
    // Work RAM from `offset` on as it was when the checkpoint opened. The
    // range must be covered by the block.
    void read_checkpoint(std::uint32_t offset, std::span<std::uint8_t> output) const noexcept {
        if (imaged) {
            std::copy_n(image.data() + (offset - first), output.size(), output.data());
            return;
        }
        std::copy_n(base + (offset - first), output.size(), output.data());
        rewind(offset, output);
    }
    // Every byte of the block is known to its mirror again.
    void clear() noexcept {
        seen = {}; kept = {}; dirty = {};
        all = false;
    }

    // Whether the loop hears of stores to both serviced bytes: the first must
    // be the first byte of work RAM, the second (it may be absent) must lie
    // in the trap chunk.
    [[nodiscard]] bool traps(const std::uint8_t* one, const std::uint8_t* two) const noexcept {
        if (base == nullptr || one != base - first) return false;
        return two == nullptr || (two >= base && two < base + size
            && (static_cast<std::uint32_t>(two - base) >> chunk_bits) == trap);
    }
    // The caller services this byte of the block (null: none). It must not
    // be the first byte of its chunk: the chunk before would reach it.
    void set_trap(const std::uint8_t* at) noexcept {
        trap = base == nullptr || at == nullptr || at <= base || at >= base + size
                || ((at - base) & (chunk_size - 1U)) == 0U
            ? ~std::uint32_t{0} : static_cast<std::uint32_t>(at - base) >> chunk_bits;
        seen = {};
    }
    // Whether the marks cover these bytes of work RAM (offsets from $7e0000).
    [[nodiscard]] bool covers(std::uint32_t offset, std::uint32_t count) const noexcept {
        return base != nullptr && offset > first && offset + count <= first + size;
    }
    // Whether any chunk that can reach these bytes is marked.
    [[nodiscard]] bool any(const Marks& marks, std::uint32_t offset, std::uint32_t count) const noexcept {
        for (std::uint32_t chunk = (offset - 1U - first) >> chunk_bits;
             chunk <= (offset + count - 1U - first) >> chunk_bits; ++chunk)
            if (marks[chunk] != 0U) return true;
        return false;
    }
};

} // namespace starfox::simulation::detail
