#include "c3d_units_3ds.h"
#include <stddef.h>

// Citro3D's single context object. Only its texture array is touched, and
// only after it has been found by value: no field offset is assumed.
extern void* __C3D_Context[];

// In 1.7.1 on ARM, tex[3] is at byte 280 of the 720-byte context. Search
// only inside the object.
enum { CONTEXT_SCAN_WORDS = 160 };

bool starwing_release_texture_units(const void* unit0, const void* unit1,
    const void* unit2) {
    if (!unit0 || !unit1 || !unit2) return false;
    void** words = __C3D_Context;
    for (size_t index = 0; index + 2 < CONTEXT_SCAN_WORDS; ++index) {
        if (words[index] != unit0 || words[index + 1] != unit1
            || words[index + 2] != unit2) continue;
        words[index + 1] = NULL;
        words[index + 2] = NULL;
        return true;
    }
    return false;
}
