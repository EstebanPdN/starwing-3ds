#include "../platform/3ds/source/diagnostic_sha256.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::string digest(std::span<const std::uint8_t> bytes, std::size_t chunk) {
    starfox::platform_3ds::DiagnosticSha256 sha;
    sha.update({});
    while (!bytes.empty()) {
        const auto count = std::min(chunk, bytes.size());
        sha.update(bytes.first(count));
        bytes = bytes.subspan(count);
        sha.update({});
    }
    std::string result;
    constexpr char hex[] = "0123456789abcdef";
    for (auto byte : sha.finish()) {
        result += hex[byte >> 4U];
        result += hex[byte & 15U];
    }
    return result;
}

void check(const std::string& input, const char* expected) {
    const auto bytes = std::span{reinterpret_cast<const std::uint8_t*>(input.data()), input.size()};
    for (const auto chunk : {1U, 7U, 55U, 56U, 63U, 64U, 65U, 4096U, 16384U})
        if (digest(bytes, chunk) != expected)
            throw std::runtime_error("SHA256 known-vector mismatch");
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        std::ifstream file(argv[1], std::ios::binary);
        if (!file) return 2;
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{file}, {}};
        const auto chunk = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 16384U;
        if (chunk == 0U) return 2;
        std::cout << digest(bytes, chunk) << '\n';
        return 0;
    }
    check("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    check(std::string(1'000'000U, 'a'),
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    std::cout << "SHA256: four known vectors, nine chunk sizes, empty updates passed\n";
}
