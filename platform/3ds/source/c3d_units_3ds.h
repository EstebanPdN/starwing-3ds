#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
// Citro3D 1.7.1 cannot unbind texture units 1 and 2: C3D_TexBind reads
// tex->param before it stores the pointer, so passing NULL faults. A bound
// unit then stays enabled for every later draw, including Citro2D's, whose
// shader outputs no texcoord2. Clear units 1 and 2 after a pass that bound
// exactly (unit0, unit1, unit2). Returns false, changing nothing, unless the
// three pointers are found consecutively in the context; the caller must
// then stop using units 1 and 2. Rebind unit 0 afterwards so Citro3D
// rewrites GPUREG_TEXUNIT_CONFIG.
bool starwing_release_texture_units(const void* unit0, const void* unit1,
    const void* unit2);
#ifdef __cplusplus
}
#endif
