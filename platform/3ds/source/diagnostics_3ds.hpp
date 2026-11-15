#pragma once

#include <3ds.h>
#include <cstddef>
#include <cstdint>
#include <string>

// Diagnostics3ds: the capture and clear button chords and the SD-card diagnostic captures under
// sdmc:/3ds/Starwing/dumps. Used by game_3ds.cpp; the 3DS test program
// tests/diagnostic_capture_3ds_test.cpp runs capture() while large allocations fail.

namespace starfox::platform_3ds {

// All strings must describe the current session and contain no ROM/save bytes.
// Caller owns the storage until capture() returns.
struct DiagnosticReport3ds {
    using ProgressCallback = void (*)(void*, const char*, u64, u64);
    const char* build{"unknown"};
    const char* build_fingerprint{"unavailable"};
    const char* runtime_json{"{}\n"};
    const char* performance_csv{"metric,value\n"};
    const char* presentation_csv{"unavailable\n"};
    const char* renderer_text{"unavailable\n"};
    const char* audio_text{"unavailable\n"};
    const char* config_text{"unavailable\n"};
    const char* memory_text{"unavailable\n"};
    const char* flow_profile_text{"unavailable\n"};
    // Private, focused RAM snapshots for exact host replay and BG2 analysis.
    // These contain live game RAM; they are separate from public text stats.
    const void* game_state{};
    std::size_t game_state_bytes{};
    const void* ppu_state{};
    std::size_t ppu_state_bytes{};
    const void* spc_state{};
    std::size_t spc_state_bytes{};
    const char* log_path{nullptr}; // Optional bounded tail of a known text log.
    // Last framebuffer actually submitted for scanout. gfxGetFramebuffer()
    // can point at the next drawing buffer after a double-buffer swap.
    // Keep these stable until capture() returns; nullptr uses gfx fallback.
    const u8* top_framebuffer{nullptr};
    const u8* bottom_framebuffer{nullptr};
    bool framebuffer_snapshots{}; // CPU copies made before progress replaces the LCD.
    // Full captures only: (progress_context, stage label, bytes written,
    // total bytes) as the stages and the memory dump advance.
    ProgressCallback progress_callback{};
    void* progress_context{};
};

// quick: reports, screenshots and RAM snapshots; full: also every readable
// process memory region.
enum class DiagnosticKind3ds { quick, full };

class Diagnostics3ds {
public:
    // Call after hidScanInput(). Returns at most one edge-triggered request.
    // L+R+B takes priority if both action buttons arrive in one scan.
    // L+R+A asks for a quick capture and L+R+B for a full one: `requested`
    // is set on the press edge and the result names the kind.
    DiagnosticKind3ds poll_shortcut(u32 down, u32 held, bool& requested) noexcept;
    // L+R+X removes only generated captures under the fixed dumps directory.
    // True once per press of the chord; the caller then runs clear_dumps().
    bool poll_clear_shortcut(u32 down, u32 held) noexcept;
    // Touch rectangle belongs to the caller's bottom-screen UI; use its
    // actual pixel bounds. It is edge-triggered on touch-down.
    bool poll_touch(u32 down, const touchPosition& touch,
                    int x, int y, int width, int height) noexcept;
    // Captures physical gfx framebuffers. The caller must finish/synchronize
    // GPU presentation first and show result.message on screen afterwards.
    // Writes dumps/NNN-dump-<ms>-<n>-quick|full as a .partial folder that is
    // renamed only after every file, SHA256SUMS.txt and the COMPLETE marker
    // are written. On failure `path` names the incomplete folder, if any.
    // A call made while another capture or clear runs fails.
    struct Result { bool ok{}; std::string path; std::string message; };
    Result capture(DiagnosticKind3ds kind, const DiagnosticReport3ds& report);
    // Deletes the numbered and unnumbered captures under the dumps directory
    // (folders recursively, with a depth bound) and nothing else there.
    Result clear_dumps();

private:
    bool busy_{};
    bool shortcut_latched_{};
    bool clear_latched_{};
};

} // namespace starfox::platform_3ds
