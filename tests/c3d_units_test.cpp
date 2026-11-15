#include "../platform/3ds/source/c3d_units_3ds.h"
#include <cassert>
#include <cstdio>

// Stand-in for Citro3D 1.7.1's context: 720 bytes on ARM, tex[3] at byte 280.
extern "C" { void* __C3D_Context[180]; }

int main() {
    int palette = 0, checker = 0, other = 0;
    constexpr int tex = 70;
    __C3D_Context[tex] = &palette;
    __C3D_Context[tex + 1] = &palette;
    __C3D_Context[tex + 2] = &checker;
    // A lone matching word elsewhere must not be taken for the array.
    __C3D_Context[10] = &palette;
    __C3D_Context[11] = &other;
    assert(starwing_release_texture_units(&palette, &palette, &checker));
    assert(__C3D_Context[tex] == &palette);
    assert(__C3D_Context[tex + 1] == nullptr && __C3D_Context[tex + 2] == nullptr);
    assert(__C3D_Context[10] == &palette && __C3D_Context[11] == &other);
    // Already released, or bound to something else: report and change nothing.
    assert(!starwing_release_texture_units(&palette, &palette, &checker));
    __C3D_Context[tex + 1] = &other;
    __C3D_Context[tex + 2] = &checker;
    assert(!starwing_release_texture_units(&palette, &palette, &checker));
    assert(__C3D_Context[tex + 1] == &other && __C3D_Context[tex + 2] == &checker);
    assert(!starwing_release_texture_units(nullptr, &palette, &checker));
    // Beyond the bounded search the pointers are not touched.
    __C3D_Context[170] = &palette; __C3D_Context[171] = &palette; __C3D_Context[172] = &checker;
    __C3D_Context[tex] = nullptr;
    assert(!starwing_release_texture_units(&palette, &palette, &checker));
    assert(__C3D_Context[171] == &palette);
    std::puts("PASS: c3d_units");
}
