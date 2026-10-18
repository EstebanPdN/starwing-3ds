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
