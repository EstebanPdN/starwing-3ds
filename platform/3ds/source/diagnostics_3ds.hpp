#pragma once

#include <3ds.h>
#include <cstddef>
#include <cstdint>
#include <string>

namespace starfox::platform_3ds {

// All strings must describe the current session and contain no ROM/save bytes.
// Caller owns the storage until capture() returns.
struct DiagnosticReport3ds {
    using ProgressCallback = void (*)(void*, const char*, u64, u64);
    const char* build{"unknown"};
    const char* runtime_json{"{}\n"};
    const char* performance_csv{"metric,value\n"};
    const char* renderer_text{"unavailable\n"};
    const char* audio_text{"unavailable\n"};
    const char* config_text{"unavailable\n"};
    const char* log_path{nullptr}; // Optional bounded tail of a known text log.
    // Last framebuffer actually submitted for scanout. gfxGetFramebuffer()
    // can point at the next drawing buffer after a double-buffer swap.
    // Keep these stable until capture() returns; nullptr uses gfx fallback.
    const u8* top_framebuffer{nullptr};
    const u8* bottom_framebuffer{nullptr};
    bool framebuffer_snapshots{}; // CPU copies made before progress replaces the LCD.
    ProgressCallback progress_callback{};
    void* progress_context{};
};

enum class DiagnosticKind3ds { quick, full };

class Diagnostics3ds {
public:
    // Call after hidScanInput(). Returns at most one edge-triggered request.
    // L+R+B takes priority if both action buttons arrive in one scan.
    DiagnosticKind3ds poll_shortcut(u32 down, u32 held, bool& requested) noexcept;
    // L+R+X removes only generated captures under the fixed dumps directory.
    bool poll_clear_shortcut(u32 down, u32 held) noexcept;
    // Touch rectangle belongs to the caller's bottom-screen UI; use its
    // actual pixel bounds. It is edge-triggered on touch-down.
    bool poll_touch(u32 down, const touchPosition& touch,
                    int x, int y, int width, int height) noexcept;
    // Captures physical gfx framebuffers. The caller must finish/synchronize
    // GPU presentation first and show result.message on screen afterwards.
    struct Result { bool ok{}; std::string path; std::string message; };
    Result capture(DiagnosticKind3ds kind, const DiagnosticReport3ds& report);
    Result clear_dumps();

private:
    bool busy_{};
    bool shortcut_latched_{};
    bool clear_latched_{};
};

} // namespace starfox::platform_3ds
