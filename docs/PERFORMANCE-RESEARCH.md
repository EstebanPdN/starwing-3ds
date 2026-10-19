# Performance research for 1.1

Read the Nintendo 3DS optimization cases and selected architecture/audit sections of [Code Atlas](https://code-atlas.estebanpdn.chatgpt.site/nintendo-3ds-manual.html). The applied principles are independent model budgets, a measured critical path, explicit GPU ownership, bounded buffers, exact cache identity and differential rendering/audio checks. This is selected chapter/source review, not a claim to have read every chapter or benchmarked every port.

## Primary implementation review

Each link pins the source snapshot used. Scope is the linked module or relevant functions, not every file in the project. No performance claim is inferred from another project.

| Project / source | Relevance and decision |
| --- | --- |
| [XProger/OpenLara](https://github.com/XProger/OpenLara/blob/3b268ca4bf986c380f44a26ffd9218bf6d376249/src/gapi/c3d.h#L25-L77) | Packed short/byte vertex layout and explicit VRAM/linear ownership informed the 8-byte span layout. |
| [TheXTech/TheXTech](https://github.com/TheXTech/TheXTech/blob/5f0745fcb74028410f45ede10983580b1e31ef6d/src/core/3ds/render_3ds.cpp#L690-L800) | Layered offscreen composition is relevant; no claim that its layer renderer directly matches Starwing. |
| [endercodezz/P3P3DS](https://github.com/endercodezz/P3P3DS/blob/4d198db9a4591ea5d5bb171c4fec2ec3e9d8585c/core/include/p3p3ds/ge/vertex_cache.hpp#L1-L144) | Exact raw-byte identity and volatile-source cache bypass; useful for future model cache work, not copied. |
| [ClassiCube/ClassiCube](https://github.com/ClassiCube/ClassiCube/blob/aa47929825a4647354588818fb0b002b2f808279/src/3ds/Graphics_3DS.c#L160-L172) | Persistent targets and explicit stereo resource ownership; Old mono allocation is now separate. |
| [masterfeizz/Quake2CTR](https://github.com/masterfeizz/Quake2CTR/blob/993114e95e368f73b48ddf027dcf061067f97913/ctr/heap_ctr.c#L1-L68) | Resource-limit-aware heap division; larger requested heaps do not create physical Old-model memory. |
| [masterfeizz/ioQuake3DS](https://github.com/masterfeizz/ioQuake3DS/blob/6e8f20d926d48d780ad24ff21876e392dcd22bfa/code/ctr/ctr_heap.c#L1-L68) | Heap split is a memory-budget precedent; no wholesale allocator replacement was made. |
| [MrHuu/fallout1-ce-3ds](https://github.com/MrHuu/fallout1-ce-3ds/blob/fdb9cd7eca0d03191019c410f084196b6e1df972/src/platform/ctr/ctr_gfx.cc#L497-L590) | 3DS presentation adapter inspected as a platform boundary; no renderer substitution was made. |
| [diasurgical/devilutionX](https://github.com/diasurgical/devilutionX/blob/b0a3b7babbdd1c10b2f7f946395e251783ae2134/CMake/platforms/n3ds.cmake#L1-L64) | Platform compile options and fixed file buffers; no automatic CPU gain follows from these flags. |
| [paaime/ac-3ds](https://github.com/paaime/ac-3ds/blob/438363b884d14802aeea53a595dbe1c31876481e/platform/3ds/gfx/gx_tev_cache.c#L29-L89) | Normalized combiner keys use hash plus exact equality; future state-cache precedent, not a new cache here. |
| [iNdioNicarao/dxx-3ds](https://github.com/iNdioNicarao/dxx-3ds/blob/56db93376191dc9e899c2993b25fdb0f62c4c26d/d1/main/render.c#L80-L91) | Geometry and texture-merge paths illustrate avoiding repeated conversion; no direct code transfer. |
| [MiniBandit/MKW-3DS](https://github.com/MiniBandit/MKW-3DS/blob/d9d017c62e49c2b39e6dadc2c55de8c0dafb4945/src/render_thread.cpp#L1-L19) | Deferred render command ownership copies mutable state; not enabled on constrained Old CPU cores. |
| [2gifts/smash64-3ds](https://github.com/2gifts/smash64-3ds/blob/780bf3f9eff05270f72309d4dca7b4628016fdff/3ds/src/render_thread.c#L6-L55) | New-only worker and game-memory/translation fences; single GPU context ownership retained. |
| [mkst/sm64-port](https://github.com/mkst/sm64-port/blob/3ds-port/README.md) | Reference-audio switches and affinity fallbacks are useful evidence controls; inaccurate math not adopted. |
| [snesrev/zelda3](https://github.com/snesrev/zelda3/blob/master/src/spc_player.c) | Native C SPC driver avoids instruction interpretation; Starwing uses guarded block lifting instead of pretending the drivers are interchangeable. |

## Community audio follow-up

[Snipy44300 Community Patches #49](https://github.com/EstebanPdN/zelda-alttp-3ds/issues/49) provides concrete DSP and retained-background changes. Patches 0028/0056 were read in full for the audio assessment; their advertised instruction reductions are kept separate from console elapsed time. See [Native engine](NATIVE-ENGINE.md) for the transfer matrix, archive identity, measurements and remaining proofs.

The complete measured result must include simulation, SPC compute/wait, renderer preparation, cache cleaning, GPU submission/execution, presentation gaps and SD I/O. Improving one stage is not sufficient evidence of a faster game.
