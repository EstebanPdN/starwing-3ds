# Renderer investigation for the 1.2 candidate

This is a source-based investigation of the [NP49 case study](https://code-atlas.estebanpdn.chatgpt.site/nintendo-3ds-manual.html#np49), the implementations below, and Starwing's own measured paths. It is not a benchmark of Mario Kart Wii or Mario Kart 64, and emulator timings are not physical Old/New 3DS results. Current validation and remaining acceptance are recorded in [STATUS.md](STATUS.md).

## Source snapshots and architectural differences

The main MKW comparison uses MiniBandit/MKW-3DS commit `f89d817ee44b1781bbe4e08dd853b92b38233376`. Epic's comparison uses Epic0522/MarioKart64-3DSPort-Complete commit `c7d3da5e864b443a69a53cc98fa24d0a14dbbac1`. The linked files, shader programs, state transitions and queue ownership were inspected; this does not claim every file or gameplay path was audited. The earlier [research matrix](PERFORMANCE-RESEARCH.md) retains the other reviewed ports and their pinned revisions.

| Component | MKW-3DS | Epic's MK64 port | Starwing / adaptation |
| --- | --- | --- | --- |
| Input to rendering | Converted game assets and native ARM game logic, with retained mesh data | Fast3D display-list state and clip-space vertices translated to Citro3D | Native scene preparation plus the cartridge's CPU/SPC logic and exact integer raster rules |
| Vertex work | 44-byte packed vertices; vertex shader performs transforms, skinning, UV and lighting work | Custom passthrough shader preserves clip W; optional GPU X/Y transforms retain CPU Z/W | Existing 8-byte span vertices encode screen rectangle coordinates, two palette indices and checker scale |
| Fragment work | PICA fixed-function texture combiners/materials, not a programmable fragment shader | Cached TEV configurations translate N64 combiners | Palette textures and a repeating checker texture select exact ordered span colours using TEV |
| Visibility | Conservative AABB subdivision of static meshes, preserving range/order constraints | Visibility/order inherited from the display-list translation | Ordered 2D spans already encode coverage; arbitrary mesh culling or reordering would change painter order |
| Submission | A render worker owns the GPU context; two lists retain copied mutable state through completion | Buffered vertices, stable texture objects and explicit dirty buffer handling | Main thread remains the GPU owner; audio and eligible background planning have separate CPU work |
| Old-model policy | Reduced detail/work cadence and a render cadence distinct from simulation | Different source workload and translation costs | Preserve Starwing's features; reduce repeated CPU work and redundant submissions without adopting another game's quality cuts |

### What MKW actually does

[`mkw.v.pica`](https://github.com/MiniBandit/MKW-3DS/blob/f89d817ee44b1781bbe4e08dd853b92b38233376/src/mkw.v.pica) handles a 19-bone palette occupying 57 vector uniforms. Its skinning/normal transform and lighting paths move substantial mesh work off ARM. The normal transformation accounts for the transform's cofactor; blindly applying a position matrix to normals would not be equivalent. This is useful for a mesh engine, but Starwing's submitted spans have already passed its integer projection and visibility rules. Moving those rules to PICA float24 requires a new correctness proof and cannot be justified by the existence of MKW's shader.

[`renderer.cpp`](https://github.com/MiniBandit/MKW-3DS/blob/f89d817ee44b1781bbe4e08dd853b92b38233376/src/renderer.cpp) retains converted vertex data, groups adjacent compatible material work, and uses bounded conservative visibility ranges. Its static visibility blocks contain 128 triangles, with a small bounded set of draw ranges. This avoids re-decoding and unnecessary draws without globally sorting away transparency order. Starwing already batches its ordered span list into one world draw. Its expensive preparation also includes source simulation, cartridge background handling and SPC execution, so adding a mesh draw sorter would target the wrong work.

[`render_thread.cpp`](https://github.com/MiniBandit/MKW-3DS/blob/f89d817ee44b1781bbe4e08dd853b92b38233376/src/render_thread.cpp) copies mutable submission state and keeps buffers owned until the GPU is finished. A pointer to a mutable game object is not a safe deferred command. Starwing keeps one GPU owner and does not spend Old 3DS's constrained second core on an additional render thread while its audio worker needs that budget.

The [Citro3D hardware patch](https://github.com/MiniBandit/MKW-3DS/blob/f89d817ee44b1781bbe4e08dd853b92b38233376/patches/citro3d-hardware.patch) is particularly relevant: queue idleness and completion of the queue's screen-swap callback are distinct events. Reusing the framebuffer before that callback finishes is a race. Starwing implements its own small public-API/linker wrapper, rather than importing the foreign patch or assuming a private Citro3D context layout. It also keeps explicit cache cleaning of CPU-written device buffers and command lists.

The [quality policy](https://github.com/MiniBandit/MKW-3DS/blob/f89d817ee44b1781bbe4e08dd853b92b38233376/include/mkw/render_quality.hpp) and [presentation cadence](https://github.com/MiniBandit/MKW-3DS/blob/f89d817ee44b1781bbe4e08dd853b92b38233376/include/mkw/presentation_cadence.hpp) matter when interpreting performance: the inspected Old policy includes a 1500-unit LOD range, 48 particles, animation work every four ticks, disabled shadows and presentation every two source ticks. It is not evidence that every visual feature runs unchanged at 60 FPS on Old 3DS. Those quality reductions are not adopted here.

### What Epic's fork does differently

[`fast3d_passthrough.v.pica`](https://github.com/Epic0522/MarioKart64-3DSPort-Complete/blob/c7d3da5e864b443a69a53cc98fa24d0a14dbbac1/platform/3ds/shaders/fast3d_passthrough.v.pica) keeps the clip-space W required for perspective interpolation. Its optional GPU matrix route changes X/Y while retaining CPU Z/W to avoid depth changes from PICA precision. It also implements UV scaling/clamping, fog and optional native lighting. A shader called “passthrough” is therefore not proof that all the renderer's work stays on the CPU.

[`gfx_citro3d.cpp`](https://github.com/Epic0522/MarioKart64-3DSPort-Complete/blob/c7d3da5e864b443a69a53cc98fa24d0a14dbbac1/platform/3ds/source/gfx_citro3d.cpp) retains stable texture handles in a deque, caches the six TEV stages and flushes the used vertex ranges. It accounts for Citro2D buffers too, with a conservative whole-linear-memory fallback. The transferable lesson is to track every writer and invalidate state across subsystem handoffs. Its N64 texture/material cache is not a drop-in cache for mutable SNES VRAM, palette/brightness, per-line scroll and coverage.

## Starwing changes and their contracts

- **GPU lifetime:** acquire waits include the queue callback, empty queues do not accidentally arm automatic submission, suspend waits are bounded, and an abandoned GPU retains all memory it could still access. No synthetic completion interrupt is generated. Recovery serializes with an already-entered swap callback and uses independent scanout buffers, so neither a late swap nor a late transfer can overwrite it. Those two buffers reserve 375 KiB of linear memory for the process lifetime. Retrying a scene after a recoverable presentation problem is different from reusing an abandoned GPU context. The bootstrap console's single-buffer mode is also explicitly reset before GPU presentation.
- **Colour-only targets:** the BG2 offscreen target has no depth buffer. Its pass explicitly disables depth testing/writes, flushes the queued 2D vertices, then restores the screen pass's state. Azahar Software exposed an otherwise hidden read of the missing depth attachment.
- **Checker shader:** an integer 32-period offset keeps checker UVs positive across the visible 240-line raster. Repeat phase and instruction count are unchanged, while the Software backend no longer selects the opposite dither texel through negative-coordinate rounding. Both backends are checked against the integer span reference.
- **4:3 presentation:** all layers use the same central 256×224 source window, scaled to 320×240 at horizontal position 40 on the 400×240 screen. The crop applies once to the composition and the HUD stays within it. A 1/32 destination-pixel sampling bias resolves exact texel-boundary float24 rounding consistently with the integer reference. Panoramic rendering is unchanged.
- **CPU backgrounds:** eligible BG2 rows resolve a tile run once, while preserving HDMA, tile flips, priorities, transparency and the ground continuation. Power-of-two tilemap wrapping uses a mask instead of repeated ARM11 division. Mosaic, tunnel and special art retain their established paths.
- **CPU/SPC scheduling:** instruction counter updates used solely on the emulation thread need no cross-thread sequential-consistency barrier. Old's SPC worker attempts core 1 with 80/70/50 percent budget fallbacks, then uses synchronous execution if unavailable. Source/audio clocks and audio features are retained. Interpolation preparation is omitted when the game is already behind its source clock; this is frame scheduling, not a reduction of game logic.
- **Stereo:** world disparity remains inward, with a screen reference plane at 256 source units and strength clamped to eight pixels per eye. Previously, BG2 mountains could remain at zero disparity while nearer 3D objects receded behind them. BG2 now uses the corresponding far-plane shift in each eye, including the GPU background route and edge coverage. BG2 is a flat source image, so this is a far-plane approximation, not recovered per-mountain geometry. HUD disparity remains zero. Physical comfort/occlusion checks remain necessary.

## Validation discipline

The owner reproduced a New 3DS freeze on HOME after the initial candidate and
both attempted HOME corrections. The failure remains unresolved in 1.2-E2.
See the [hardware report](STATUS.md#physical-home-failure-remains-unresolved).
The retained image/timing results below precede the lifecycle follow-ups;
they do not establish HOME acceptance for any candidate.

Azahar Software is a diagnostic backend of the emulator. It does not change the release's PICA renderer. Autopilot scripts, slider overrides, injected GPU failures and paired image readback are compiled only in regression builds.

The local Azahar 2126.2 ARM64 Software diagnostic uses one raster worker, selected at the renderer constructor through LLDB and verified in its run log. The normal multiworker backend crashed in texture memory lookup; the one-worker run avoids that emulator issue. This restriction is confined to the test process, and those timings are never used for performance claims. Vulkan remains unmodified. Its raw cached framebuffer-memory download also produced stripes in the integrated run while an explicit render-target transfer and the visible application window were correct; final-target and scanout captures are recorded separately.

Each visual capture compares the final render target or scanout with a CPU reconstruction of the same simulation, PPU and interpolation state. Captures are outside the throughput measurement window. A ROM-free presenter executable separately exercises patterned texture upload, translucent foreground coverage, palette/dither spans at scales 1 through 4, BG2 composition, repeated frame handoffs, independent CPU recovery and panoramic/4:3 views. Its final sixteen cases compare 1,536,000 pixels per backend with zero differences under both Software and Vulkan.

The host suite checks the GPU callback race, bounded waits and permanent quarantine, viewport sampling and stereo bounds, and compares optimized BG2 output with the per-pixel renderer over both source and wide rasters. Native SPC comparisons check full state and PCM, not just audible similarity. Emulator Old/New profiles, actual graphics backend, binary hash, script and options are recorded with each run. The macOS Azahar build falls back from an unavailable OpenGL request to Vulkan; historical folders named `gl` must not be reported as OpenGL measurements.

### Integrated image and recovery evidence

| Profile and route | Compared output | Result |
| --- | --- | --- |
| Old, 4:3, Software | Intro/Corneria captures at source ticks 450, 750 and 950; scanout and explicit final target checked separately | Zero differences in each set of 288,000 pixels |
| New, Wide stereo, Vulkan | Both final eye targets at ticks 750 and 950 | Zero differences in 384,000 pixels; HUD remains on the screen plane |
| Old, Wide, Vulkan | Final target at tick 1480, after the benchmark window | Zero differences in 96,000 pixels |
| Old, injected GPU presentation failures | Switch to the reference renderer at tick 604, then independent CPU scanout at 610; captures at 900 and 1250 | Run reaches tick 1300; zero differences in 192,000 recovery pixels |

The injected-failure run tests the watchdog and continued presentation; it does not reproduce a physically wedged PICA. Host concurrency tests separately model a late callback and writes to old GPU buffers. The two eyes matching the reference proves the implemented disparity, not subjective stereo comfort. The final ROM-free run includes the last initialization cleanup, which keeps both CPU screen formats valid if recovery-buffer allocation fails. The integrated binaries precede that failure-only cleanup; their normal rendering path is unchanged.

### Wide Mode timing evidence

All further performance measurements use Wide Mode. The retained comparison is one sequential run per binary under Azahar 2126.2 Vulkan's Old profile, with top HUD enabled, no save writes and the same scripted source-tick inputs. It compares gameplay ticks 600–1450, after startup and before the image capture at 1480. Baseline 1.2-E1 contributes 58 one-second rows; the candidate contributes 59. The candidate also logs Mode 2 explicitly; the older baseline lacks that column and is matched by the same scripted route. These are medians of the recorded windows, not repeated-trial confidence estimates.

| Metric | 1.2-E1 | 1.2 candidate |
| --- | ---: | ---: |
| Presented FPS | 23.715 | 31.000 |
| Presentation interval | 42.17 ms | 32.26 ms |
| Game work | 18.71 ms | 19.00 ms |
| Draw preparation | 8.455 ms | 8.390 ms |
| Upload/presentation work | 5.35 ms | 3.83 ms |
| Bottom-screen work | 1.95 ms | 1.46 ms |
| Median of each window's work p95 | 54.82 ms | 38.72 ms |
| Source logic rate | 14.845 Hz | 15.000 Hz |
| Free linear memory | 9,095 KiB | 8,720 KiB |

The observed presentation-rate increase is 30.7%; the median interval falls by 23.5%. It is an aggregate candidate comparison, not attribution of the whole improvement to an individual shader or cache. Simulation work remains a significant cost and the observed source rate is about 15 Hz: higher presentation FPS does not establish full source speed. The 375 KiB linear-memory difference is the independent recovery reserve.

The baseline autopilot image SHA-256 is `565ceb04abf01b5c921de8988e5e7898ca110681ee403010d52aa1b85fb3b432`; the measured candidate is `4b9e955b3f164ed290ca7474ddf5248e62e22e0754bc7260c42250b4b5ffded2`. These identify regression executables, not the release CIA. Per-run records retain the actual backend, script hash, options and immutable input image. The unfinished 4:3 timing run was cancelled when Wide-only performance testing was requested and is excluded from this comparison.

Hardware black-screen, HOME/sleep and stereo acceptance are not established by these host or emulator checks. The New-profile visual check is not a New 3DS speed measurement; Azahar does not reproduce the physical Old/New CPU-clock and scheduling difference. No ROM, extracted asset, private guide, save or game-derived fixture is part of this source document or a distributable test bundle.
