# ARM11 saturation follows the Community Patches #49 optimization idea.
set(clamp_anchor [=[
#define CLAMP16( io )\
{\
	if ( (int16_t) io != io )\
		io = (io >> 31) ^ 0x7FFF;\
}
]=])
string(FIND "${dsp_source}" "${clamp_anchor}" clamp_position)
if(clamp_position EQUAL -1)
    message(FATAL_ERROR "Pinned SPC saturation operation needs review")
endif()
string(REPLACE "${clamp_anchor}" [=[
#include "starfox/audio/saturate.hpp"
#define CLAMP16(io) { io = starfox::audio::saturate16(io); }
]=] dsp_source "${dsp_source}")

# The only two read_counter callers test zero (envelope and noise).
set(counter_anchor "return ((unsigned) m.counter + counter_offsets [rate]) % counter_rates [rate];")
string(FIND "${dsp_source}" "${counter_anchor}" counter_position)
if(counter_position EQUAL -1)
    message(FATAL_ERROR "Pinned DSP counter needs review")
endif()
string(REPLACE "#include \"starfox/audio/saturate.hpp\""
    "#include \"starfox/audio/saturate.hpp\"\n#include \"starfox/audio/dsp_counter.hpp\""
    dsp_source "${dsp_source}")
string(REPLACE "${counter_anchor}"
    "return !starfox::audio::dsp_counter_fires((unsigned) m.counter + counter_offsets[rate], rate);"
    dsp_source "${dsp_source}")

# Store the four interpolation coefficients together. This changes only their
# layout; retain the original per-product shifts, signed wrap and saturation.
string(REPLACE "#include \"SPC_DSP.h\"" "#include \"SPC_DSP.h\"\n#include <array>"
    dsp_source "${dsp_source}")
string(REPLACE "static short const gauss [512] =" "static constexpr short gauss [512] ="
    dsp_source "${dsp_source}")
set(interpolate_anchor "inline int SPC_DSP::interpolate( voice_t const* v )")
string(FIND "${dsp_source}" "${interpolate_anchor}" interpolate_position)
if(interpolate_position EQUAL -1)
    message(FATAL_ERROR "Pinned SPC interpolation needs review")
endif()
string(REPLACE "${interpolate_anchor}" [=[
static constexpr auto gauss_rows = [] {
    std::array<std::array<short, 4>, 256> rows{};
    for (unsigned i = 0; i < rows.size(); ++i)
        rows[i] = {gauss[255-i], gauss[511-i], gauss[256+i], gauss[i]};
    return rows;
}();

inline int SPC_DSP::interpolate( voice_t const* v )
]=] dsp_source "${dsp_source}")
set(gauss_pointers [=[
	short const* fwd = gauss + 255 - offset;
	short const* rev = gauss       + offset; // mirror left half of gaussian
]=])
string(FIND "${dsp_source}" "${gauss_pointers}" gauss_position)
if(gauss_position EQUAL -1)
    message(FATAL_ERROR "Pinned SPC coefficient access needs review")
endif()
string(REPLACE "${gauss_pointers}" "\tconst auto& coefficients = gauss_rows[offset];\n"
    dsp_source "${dsp_source}")
string(REPLACE "fwd [  0]" "coefficients[0]" dsp_source "${dsp_source}")
string(REPLACE "fwd [256]" "coefficients[1]" dsp_source "${dsp_source}")
string(REPLACE "rev [256]" "coefficients[2]" dsp_source "${dsp_source}")
string(REPLACE "rev [  0]" "coefficients[3]" dsp_source "${dsp_source}")

# Preserve the DSP schedule and BRR buffer layout while decoding one group
# with a shared range/filter decision and register-held predictor history.
string(FIND "${dsp_source}" "inline void SPC_DSP::decode_brr( voice_t* v )" brr_begin)
string(FIND "${dsp_source}" "//// Misc" brr_end)
if(brr_begin EQUAL -1 OR brr_end LESS brr_begin)
    message(FATAL_ERROR "Pinned SPC BRR decoder needs review")
endif()
math(EXPR brr_size "${brr_end} - ${brr_begin}")
string(SUBSTRING "${dsp_source}" ${brr_begin} ${brr_size} brr_original)
string(REPLACE "${brr_original}" [=[
#include "starfox/audio/brr_decode.hpp"
inline void SPC_DSP::decode_brr( voice_t* v )
{
    const unsigned nybbles = m.t_brr_byte * 256U
        + m.ram[(v->brr_addr + v->brr_offset + 1) & 65535U];
    int* const pos = &v->buf[v->buf_pos];
    if ((v->buf_pos += 4) >= brr_buf_size) v->buf_pos = 0;
    starfox::audio::decode_brr_four(nybbles, m.t_brr_header, pos);
}


]=] dsp_source "${dsp_source}")

# Inline these steps into their existing composite voice functions; do not
# duplicate the complete synthesizer or change any clock/event boundaries.
string(REPLACE "#define VOICE_CLOCK( n ) void SPC_DSP::voice_##n( voice_t* const v )" [=[
#if defined(__GNUC__) || defined(__clang__)
#define STARFOX_DSP_INLINE __attribute__((always_inline)) inline
#else
#define STARFOX_DSP_INLINE inline
#endif
#define VOICE_CLOCK( n ) void SPC_DSP::voice_##n( voice_t* const v )
]=] dsp_source "${dsp_source}")
foreach(voice_step V3c V4)
    set(voice_anchor "\nVOICE_CLOCK( ${voice_step} )\n")
    string(FIND "${dsp_source}" "${voice_anchor}" voice_position)
    if(voice_position EQUAL -1)
        message(FATAL_ERROR "Pinned SPC ${voice_step} needs review")
    endif()
    string(REPLACE "${voice_anchor}" "\nSTARFOX_DSP_INLINE VOICE_CLOCK( ${voice_step} )\n"
        dsp_source "${dsp_source}")
endforeach()

# Every main/echo accumulator starts at zero or a restored int16, and each
# addition saturates it to int16. Adding a zero voice therefore changes no
# value. Preserve all voice clocks, envelopes, BRR and register publication;
# only omit this pure zero addition and its redundant saturation/stores.
set(voice_output_anchor "inline void SPC_DSP::voice_output( voice_t const* v, int ch )\n{")
string(FIND "${dsp_source}" "${voice_output_anchor}" voice_output_position)
if(voice_output_position EQUAL -1)
    message(FATAL_ERROR "Pinned SPC voice mixer needs review")
endif()
string(REPLACE "${voice_output_anchor}"
    "${voice_output_anchor}\n\tif (!m.t_output) return;"
    dsp_source "${dsp_source}")
