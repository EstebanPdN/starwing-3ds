#include "../platform/3ds/source/launch_selection.hpp"
#include <cassert>
#include <filesystem>
#include <unistd.h>
int main() {
    using namespace starfox::platform_3ds;
    char temporary[] = "/tmp/starwing-selection-XXXXXX";
    const int fd = mkstemp(temporary); assert(fd >= 0); close(fd);
    assert(read_launch_selection(temporary).filename.empty());
    assert(write_launch_selection(temporary, {"Star Fox.sfc", true}));
    const auto a = read_launch_selection(temporary);
    assert(a.filename == "Star Fox.sfc" && a.ex);
    assert(selection_available(a, {"Another.smc", "Star Fox.sfc"}));
    assert(!selection_available(a, {"Another.smc"}));
    assert(write_launch_selection(temporary, {"Another.smc", false}));
    assert(read_launch_selection(temporary).filename == "Another.smc");
    assert(!read_launch_selection(temporary).ex);
    for (const auto* bad : {"../rom.sfc", "a/b.sfc", "a\\b.sfc", "a\nb.sfc", "", "sdmc:rom.sfc"})
        assert(!write_launch_selection(temporary, {bad, false}));
    assert(read_launch_selection(temporary).filename == "Another.smc");
    std::remove(temporary);
}
