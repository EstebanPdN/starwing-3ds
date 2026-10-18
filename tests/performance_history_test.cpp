#include "../platform/3ds/source/performance_history.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

using starfox::platform_3ds::PerformanceHistory;

void require(bool value) { if (!value) std::abort(); }

int main() {
    auto history = std::make_unique<PerformanceHistory>();
    const auto header = history->csv();
    require(std::count(header.begin(), header.end(), ',') == 103);
    require(header.ends_with("input_moving_frames\n"));
    require(history->count == 0 && history->truncated_rows == 0);
    for (unsigned i = 0; i < 205; ++i)
        history->append((std::to_string(i) + "\n").c_str());
    std::string expected = header;
    for (unsigned i = 25; i < 205; ++i) expected += std::to_string(i) + "\n";
    require(history->csv() == expected);
    require(history->count == 180 && history->truncated_rows == 0);
    history->append("missing newline");
    history->append("");
    const std::string oversized(1280, 'x');
    history->append((oversized + "\n").c_str());
    require(history->truncated_rows == 3);
    std::cout << "PASS: CSV schema, empty export, 180-row rollover and truncation reporting\n";
}
