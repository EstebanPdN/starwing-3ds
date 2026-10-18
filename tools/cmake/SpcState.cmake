# Build a private copy of the pinned LGPL SPC sources. Preserve upstream files
# and license headers. State access requires no pointer serialization; the DSP
# shortcuts below preserve every emulated clock and internal state.
set(STARFOX_SPC_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/snes_spc")
file(MAKE_DIRECTORY "${STARFOX_SPC_SOURCE}")
file(GLOB spc_source_files "${snes_spc_SOURCE_DIR}/snes_spc/*")
foreach(source IN LISTS spc_source_files)
    if(NOT IS_DIRECTORY "${source}")
        get_filename_component(name "${source}" NAME)
        if(NOT name STREQUAL "SPC_Filter.h" AND NOT name STREQUAL "SNES_SPC.h"
                AND NOT name STREQUAL "SPC_DSP.cpp")
            configure_file("${source}" "${STARFOX_SPC_SOURCE}/${name}" COPYONLY)
        endif()
    endif()
endforeach()
function(starfox_write_spc_file name contents)
    set(destination "${STARFOX_SPC_SOURCE}/${name}")
    if(EXISTS "${destination}")
        file(READ "${destination}" previous)
        if(previous STREQUAL contents)
            return()
        endif()
    endif()
    file(WRITE "${destination}" "${contents}")
endfunction()
file(READ "${snes_spc_SOURCE_DIR}/snes_spc/SPC_Filter.h" filter_header)
set(filter_anchor "\tvoid clear();")
string(FIND "${filter_header}" "${filter_anchor}" anchor_position)
if(anchor_position EQUAL -1)
    message(FATAL_ERROR "Pinned SPC filter state extension needs review")
endif()
string(REPLACE "${filter_anchor}" [=[
    void clear();
    // Star Fox Enhanced extension: semantic fields, never object bytes.
    void save_history(int out[8]) const {
        out[0] = gain; out[1] = bass;
        for (int i = 0; i < 2; ++i) {
            out[2+i*3] = ch[i].p1;
            out[3+i*3] = ch[i].pp1;
            out[4+i*3] = ch[i].sum;
        }
    }
    void load_history(const int in[8]) {
        gain = in[0]; bass = in[1];
        for (int i = 0; i < 2; ++i) {
            ch[i].p1 = in[2+i*3];
            ch[i].pp1 = in[3+i*3];
            ch[i].sum = in[4+i*3];
        }
    }
]=] filter_header "${filter_header}")
starfox_write_spc_file("SPC_Filter.h" "${filter_header}")

file(READ "${snes_spc_SOURCE_DIR}/snes_spc/SNES_SPC.h" core_header)
set(core_anchor "\tint sample_count() const;")
string(FIND "${core_header}" "${core_anchor}" anchor_position)
if(anchor_position EQUAL -1)
    message(FATAL_ERROR "Pinned SPC carry-buffer extension needs review")
endif()
string(REPLACE "${core_anchor}" [=[
    int sample_count() const;
    // Star Fox Enhanced extension. Call only between end_frame/set_output.
    void save_output_carry(int& clocks, int& count, sample_t* samples) const {
        clocks = m.extra_clocks;
        count = int(m.extra_pos - m.extra_buf);
        for (int i = 0; i < extra_size; ++i)
            samples[i] = i < count ? m.extra_buf[i] : 0;
    }
    void load_output_carry(int clocks, int count, const sample_t* samples) {
        m.extra_clocks = clocks;
        for (int i = 0; i < extra_size; ++i) m.extra_buf[i] = samples[i];
        m.extra_pos = m.extra_buf + count;
        m.buf_begin = 0;
        m.buf_end = 0;
        dsp.set_output(0, 0);
    }
]=] core_header "${core_header}")
starfox_write_spc_file("SNES_SPC.h" "${core_header}")

# Exact accurate-DSP shortcuts: zero-envelope/noise interpolation is pure,
# and complete 32-clock groups keep the original voice/echo scheduling order.
file(READ "${snes_spc_SOURCE_DIR}/snes_spc/SPC_DSP.cpp" dsp_source)
set(gaussian_anchor [=[
	// Gaussian interpolation
	{
		int output = interpolate( v );
		
		// Noise
		if ( m.t_non & v->vbit )
			output = (int16_t) (m.noise * 2);
		
		// Apply envelope
		m.t_output = (output * v->env) >> 11 & ~1;
		v->t_envx_out = (uint8_t) (v->env >> 4);
	}
	
]=])
string(FIND "${dsp_source}" "${gaussian_anchor}" anchor_position)
if(anchor_position EQUAL -1)
    message(FATAL_ERROR "Pinned SPC gaussian shortcut needs review")
endif()
string(REPLACE "${gaussian_anchor}" [=[
	// Gaussian interpolation
	{
		// Gaussian interpolation has no state effects. Noise replaces its
		// result, and a zero envelope multiplies its result by zero.
		int output = 0;
		if ( v->env )
			output = (m.t_non & v->vbit)
				? (int16_t) (m.noise * 2) : interpolate( v );
		
		// Apply envelope
		m.t_output = (output * v->env) >> 11 & ~1;
		v->t_envx_out = (uint8_t) (v->env >> 4);
	}
	
]=] dsp_source "${dsp_source}")
set(dsp_run_anchor [=[
void SPC_DSP::run( int clocks_remain )
{
	require( clocks_remain > 0 );
	
	int const phase = m.phase;
	m.phase = (phase + clocks_remain) & 31;
	switch ( phase )
	{
	loop:
	
		#define PHASE( n ) if ( n && !--clocks_remain ) break; case n:
		GEN_DSP_TIMING
		#undef PHASE
	
		if ( --clocks_remain )
			goto loop;
	}
}
]=])
string(FIND "${dsp_source}" "${dsp_run_anchor}" anchor_position)
if(anchor_position EQUAL -1)
    message(FATAL_ERROR "Pinned SPC dsp_run shortcut needs review")
endif()
string(REPLACE "${dsp_run_anchor}" [=[
void SPC_DSP::run( int clocks_remain )
{
    require( clocks_remain > 0 );
    int const phase = m.phase;
    m.phase = (phase + clocks_remain) & 31;
    if (phase != 0)
    {
        int prefix = min( clocks_remain, 32 - phase );
        clocks_remain -= prefix;
        switch (phase)
        {
            #define PHASE( n ) if ( n && !--prefix ) break; case n:
            GEN_DSP_TIMING
            #undef PHASE
        }
    }
    while (clocks_remain >= 32)
    {
        #define PHASE( n )
        GEN_DSP_TIMING
        #undef PHASE
        clocks_remain -= 32;
    }
    if (clocks_remain != 0)
    {
        switch (0)
        {
            #define PHASE( n ) if ( n && !--clocks_remain ) break; case n:
            GEN_DSP_TIMING
            #undef PHASE
        }
    }
}
]=] dsp_source "${dsp_source}")
starfox_write_spc_file("SPC_DSP.cpp" "${dsp_source}")
