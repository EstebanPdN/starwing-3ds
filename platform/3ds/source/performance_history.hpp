#pragma once

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace starfox::platform_3ds {

// Bounded CSV history; appending never allocates.
struct PerformanceHistory {
    std::array<std::array<char, 1280>, 180> rows{};
    unsigned count{}, cursor{};
    unsigned truncated_rows{};
    void append(const char* row) {
        const auto written = std::snprintf(rows[cursor].data(), rows[cursor].size(), "%s", row);
        const auto length = std::strlen(row);
        if (written < 0 || static_cast<std::size_t>(written) >= rows[cursor].size()
            || length == 0U || row[length - 1U] != '\n') ++truncated_rows;
        cursor = (cursor + 1U) % rows.size();
        count = std::min<unsigned>(count + 1U, rows.size());
    }
    std::string csv() const {
        std::string result = "session_s,flow,fps,logic_hz,frame_ms,game_ms,draw_ms,upload_ms,audio_wait_ms,vblank_ms,work_p95_ms,work_max_ms,gpu_draw_ms,gpu_submit_ms,presented,missed,bg2_pica,bg2_fallback,world_cache_hits,world_cache_misses,heap_free_kib,linear_free_kib,audio_core,quota_percent,phase_ms,preload_ms,preloaded_shapes,audio_packets,audio_late_packets,audio_late_frames,bg2_fragments_mean,background_ms,world_ms,finish_ms,bottom_ms,hud_reused,tick_setup_ms,tick_object_ms,tick_strategy_ms,tick_view_ms,tick_draw_ms,tick_tail_ms,tick_wipe_ms,tick_dialogue_ms,tick_palette_oam_ms,tick_collision_ms,tick_flow_ms,loop_work_us,vblank_wait_us,tick_work_us,renderer_work_us,presentation_work_us,bottom_work_us,bg2_plan_us,gpu_acquire_us,gpu_build_us,cache_flush_us,display_transfer_us,gpu_submit_us,reuse_validation_us,world_cache_validation_us,world_cache_snapshot_us,hud_build_us,frame_reuse_checks,frame_reuse_hits,display_reuse_checks,display_reuse_hits,coalesced_phases,gpu_submissions,gpu_begin_failures,bg2_cache_validation_us,bg2_plan_cache_hits,bg2_plan_cache_misses,ppu_bg_cache_hits,ppu_bg_cache_misses,continue_bottom_reused,bg2_plan_wait_us,bg2_plan_worker_frames,world_prepare_us,world_raster_us,world_composite_us,native_model_us,finalize_us,heap_used_kib,heap_arena_kib,heap_peak_kib,linear_used_kib,linear_peak_kib,paused,shape_cache_bytes,shape_cache_budget,shape_cache_entries,shape_cache_evictions,bg2_raster_cache_hits,bg2_raster_cache_misses,bg2_emitted_quads_mean,bg2_raster_cache_bytes,present_gap_p50_ms,present_gap_p95_ms,present_gap_max_ms,present_gaps_over25,present_gaps_over50,input_held_or,input_moving_frames\n";
        for (unsigned n = 0; n < count; ++n)
            result += rows[(cursor + rows.size() - count + n) % rows.size()].data();
        return result;
    }
};

} // namespace starfox::platform_3ds
