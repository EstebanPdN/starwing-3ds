#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

// Save-slot file I/O through std::filesystem. Host tests use it; the 3DS
// frontend has its own storage code.
namespace starfox::state {
// Slots are replaced only after the entire temporary file was written and
// closed successfully. This is atomic replacement, not a power-loss guarantee.
// Creates missing parent directories. Throws for empty data, data above
// 64 MiB + 24 bytes, a path without a file name or an I/O error; a failed
// write leaves the existing file untouched.
void write_atomic(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);
// Reads a whole file of 1 byte to 64 MiB + 24 bytes. Throws
// std::runtime_error if it is missing, empty, oversized or changes size while
// being read.
[[nodiscard]] std::vector<std::uint8_t> read_file(const std::filesystem::path& path);
} // namespace starfox::state
