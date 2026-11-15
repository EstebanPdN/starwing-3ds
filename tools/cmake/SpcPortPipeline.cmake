# Split the pinned SPC CPU timeline from DSP synthesis without changing PCM.
# Fail at configure time if an upstream source no longer matches a patch.
function(starfox_spc_pipeline_replace old replacement)
    string(FIND "${pipeline_source}" "${old}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Pinned SPC port pipeline source needs review")
    endif()
    string(REPLACE "${old}" "${replacement}" changed "${pipeline_source}")
    set(pipeline_source "${changed}" PARENT_SCOPE)
endfunction()
configure_file("${CMAKE_CURRENT_LIST_DIR}/SpcPortPipeline.inl"
    "${STARFOX_SPC_SOURCE}/SpcPortPipeline.inl" COPYONLY)

file(READ "${STARFOX_SPC_SOURCE}/SNES_SPC.h" pipeline_source)

starfox_spc_pipeline_replace([=[
#ifndef SNES_SPC_H
#define SNES_SPC_H

#include "SPC_DSP.h"
#include "blargg_endian.h"

struct SNES_SPC {
public:
	typedef BOOST::uint8_t uint8_t;
	
]=] [=[
#ifndef SNES_SPC_H
#define SNES_SPC_H

#include "SPC_DSP.h"
#include "blargg_endian.h"
#include <vector>
#include <cstddef>
#include <cstring>

struct SNES_SPC {
public:
	typedef BOOST::uint8_t uint8_t;
	
]=])

starfox_spc_pipeline_replace([=[
	enum { timer_count = 3 };
	enum { extra_size = SPC_DSP::extra_size };
	
	enum { signature_size = 35 };
	
private:
	SPC_DSP dsp;
	
	#if SPC_LESS_ACCURATE
		static signed char const reg_times_ [256];
		signed char reg_times [256];
	#endif
]=] [=[
	enum { timer_count = 3 };
	enum { extra_size = SPC_DSP::extra_size };
	
	enum { signature_size = 35 };
	
#include "SpcPortPipeline.inl"
    SPC_DSP dsp;
	
	#if SPC_LESS_ACCURATE
		static signed char const reg_times_ [256];
		signed char reg_times [256];
	#endif
]=])

starfox_spc_pipeline_replace([=[
	void cpu_write_smp_reg ( int data, rel_time_t, int addr );
	void cpu_write_high    ( int data, int i, rel_time_t );
	void cpu_write         ( int data, int addr, rel_time_t );
	int cpu_read_smp_reg   ( int i, rel_time_t );
	int cpu_read           ( int addr, rel_time_t );
	unsigned CPU_mem_bit   ( uint8_t const* pc, rel_time_t );
	
	bool check_echo_access ( int addr );
	uint8_t* run_until_( time_t end_time );
	
	struct spc_file_t
	{
		char    signature [signature_size];
		uint8_t has_id666;
]=] [=[
	void cpu_write_smp_reg ( int data, rel_time_t, int addr );
	void cpu_write_high    ( int data, int i, rel_time_t );
	void cpu_write         ( int data, int addr, rel_time_t );
	int cpu_read_smp_reg   ( int i, rel_time_t );
	int cpu_read           ( int addr, rel_time_t );
	template<bool Predict> unsigned CPU_mem_bit   ( uint8_t const* pc, rel_time_t );
	
	bool check_echo_access ( int addr );
	template<bool Predict> uint8_t* run_until_cpu_( time_t end_time );
    uint8_t* run_until_( time_t end_time );
	
	struct spc_file_t
	{
		char    signature [signature_size];
		uint8_t has_id666;
]=])

starfox_write_spc_file("SNES_SPC.h" "${pipeline_source}")

file(READ "${STARFOX_SPC_SOURCE}/SPC_DSP.h" pipeline_source)

starfox_spc_pipeline_replace([=[
	enum { extra_size = 16 };
	sample_t* extra()               { return m.extra; }
	sample_t const* out_pos() const { return m.out; }
	void disable_surround( bool ) { } // not supported
public:
	BLARGG_DISABLE_NOTHROW
	
	typedef BOOST::int8_t   int8_t;
	typedef BOOST::int16_t int16_t;
	
	enum { echo_hist_size = 8 };
]=] [=[
	enum { extra_size = 16 };
	sample_t* extra()               { return m.extra; }
	sample_t const* out_pos() const { return m.out; }
	void disable_surround( bool ) { } // not supported
public:
    void pipeline_echo_guard(unsigned& current, unsigned& next, unsigned& length, unsigned& pointer) const {
        current=unsigned(m.t_esa)&255U;next=m.regs[r_esa];
        length=unsigned(m.echo_length);
        if(unsigned(m.echo_offset)+4U>length)length=unsigned(m.echo_offset)+4U;
        const unsigned requested=(m.regs[r_edl]&15U)*2048U;
        if(requested>length)length=requested;
        if(length<4U)length=4U;
        pointer=unsigned(m.t_echo_ptr)&65535U;
    }
    BLARGG_DISABLE_NOTHROW
	
	typedef BOOST::int8_t   int8_t;
	typedef BOOST::int16_t int16_t;
	
	enum { echo_hist_size = 8 };
]=])

starfox_write_spc_file("SPC_DSP.h" "${pipeline_source}")

file(READ "${STARFOX_SPC_SOURCE}/SNES_SPC.cpp" pipeline_source)

starfox_spc_pipeline_replace([=[

//// ROM

void SNES_SPC::enable_rom( int enable )
{
	if ( m.rom_enabled != enable )
	{
		m.rom_enabled = enable;
		if ( enable )
			memcpy( m.hi_ram, &RAM [rom_addr], sizeof m.hi_ram );
		memcpy( &RAM [rom_addr], (enable ? m.rom : m.hi_ram), rom_size );
		// TODO: ROM can still get overwritten when DSP writes to echo buffer
	}
}


]=] [=[

//// ROM

void SNES_SPC::enable_rom( int enable )
{
    pipeline_touch(0xffc0U);
	if ( m.rom_enabled != enable )
	{
		m.rom_enabled = enable;
		if ( enable )
			memcpy( m.hi_ram, &RAM [rom_addr], sizeof m.hi_ram );
        memcpy( &RAM [rom_addr], (enable ? m.rom : m.hi_ram), rom_size );
        if(pipeline_enabled_)for(unsigned i=0;i<rom_size;++i)
            pipeline_event(((rom_addr+i)<<8U)|RAM[rom_addr+i]);
		// TODO: ROM can still get overwritten when DSP writes to echo buffer
	}
}


]=])

starfox_spc_pipeline_replace([=[
		int count = (time) - (offset) - m.dsp_time;\
		if ( count >= 0 )\
		{\
			int clock_count = (count & ~(clocks_per_sample - 1)) + clocks_per_sample;\
			m.dsp_time += clock_count;\
			dsp.run( clock_count );\
		}
#else
	#define RUN_DSP( time, offset ) \
		{\
			int count = (time) - m.dsp_time;\
			if ( !SPC_MORE_ACCURACY || count )\
			{\
				assert( count > 0 );\
				m.dsp_time = (time);\
				dsp.run( count );\
			}\
		}
#endif

int SNES_SPC::dsp_read( rel_time_t time )
{
	RUN_DSP( time, reg_times [REGS [r_dspaddr] & 0x7F] );
	
	int result = dsp.read( REGS [r_dspaddr] & 0x7F );
	
	#ifdef SPC_DSP_READ_HOOK
		SPC_DSP_READ_HOOK( spc_time + time, (REGS [r_dspaddr] & 0x7F), result );
	#endif
	
	return result;
}

inline void SNES_SPC::dsp_write( int data, rel_time_t time )
{
	RUN_DSP( time, reg_times [REGS [r_dspaddr]] )
	#if SPC_LESS_ACCURATE
		else if ( m.dsp_time == skipping_time )
		{
			int r = REGS [r_dspaddr];
]=] [=[
		int count = (time) - (offset) - m.dsp_time;\
		if ( count >= 0 )\
		{\
			int clock_count = (count & ~(clocks_per_sample - 1)) + clocks_per_sample;\
			m.dsp_time += clock_count;\
			pipeline_run_dsp( clock_count );\
		}
#else
	#define RUN_DSP( time, offset ) \
		{\
			int count = (time) - m.dsp_time;\
			if ( !SPC_MORE_ACCURACY || count )\
			{\
				assert( count > 0 );\
				m.dsp_time = (time);\
				pipeline_run_dsp( count );\
			}\
		}
#endif

int SNES_SPC::dsp_read( rel_time_t time )
{
	RUN_DSP( time, reg_times [REGS [r_dspaddr] & 0x7F] );
	
	pipeline_dsp_read(REGS[r_dspaddr]&0x7fU);
    int result = dsp.read( REGS [r_dspaddr] & 0x7F );
	
	#ifdef SPC_DSP_READ_HOOK
		SPC_DSP_READ_HOOK( spc_time + time, (REGS [r_dspaddr] & 0x7F), result );
	#endif
	
	return result;
}

inline void SNES_SPC::dsp_write( int data, rel_time_t time )
{
    pipeline_dsp_write(REGS[r_dspaddr],unsigned(data));
	RUN_DSP( time, reg_times [REGS [r_dspaddr]] )
	#if SPC_LESS_ACCURATE
		else if ( m.dsp_time == skipping_time )
		{
			int r = REGS [r_dspaddr];
]=])

starfox_spc_pipeline_replace([=[
	#ifdef SPC_DSP_WRITE_HOOK
		SPC_DSP_WRITE_HOOK( m.spc_time + time, REGS [r_dspaddr], (uint8_t) data );
	#endif
	
	if ( REGS [r_dspaddr] <= 0x7F )
		dsp.write( REGS [r_dspaddr], data );
	else if ( !SPC_MORE_ACCURACY )
		dprintf( "SPC wrote to DSP register > $7F\n" );
}


]=] [=[
	#ifdef SPC_DSP_WRITE_HOOK
		SPC_DSP_WRITE_HOOK( m.spc_time + time, REGS [r_dspaddr], (uint8_t) data );
	#endif
	
	if ( REGS [r_dspaddr] <= 0x7F )
		pipeline_write_dsp( REGS [r_dspaddr], data );
	else if ( !SPC_MORE_ACCURACY )
		dprintf( "SPC wrote to DSP register > $7F\n" );
}


]=])

starfox_spc_pipeline_replace([=[
{
	if ( i < rom_size )
	{
		m.hi_ram [i] = (uint8_t) data;
		if ( m.rom_enabled )
			RAM [i + rom_addr] = m.rom [i]; // restore overwritten ROM
	}
	else
	{
		assert( RAM [i + rom_addr] == (uint8_t) data );
		RAM [i + rom_addr] = cpu_pad_fill; // restore overwritten padding
]=] [=[
{
	if ( i < rom_size )
	{
		m.hi_ram [i] = (uint8_t) data;
		if ( m.rom_enabled )
			pipeline_store(unsigned(i+rom_addr),m.rom[i]); // restore overwritten ROM
	}
	else
	{
		assert( RAM [i + rom_addr] == (uint8_t) data );
		RAM [i + rom_addr] = cpu_pad_fill; // restore overwritten padding
]=])

starfox_spc_pipeline_replace([=[
void SNES_SPC::cpu_write( int data, int addr, rel_time_t time )
{
	MEM_ACCESS( time, addr )
	
	// RAM
	RAM [addr] = (uint8_t) data;
	int reg = addr - 0xF0;
	if ( reg >= 0 ) // 64%
	{
		// $F0-$FF
		if ( reg < reg_count ) // 87%
]=] [=[
void SNES_SPC::cpu_write( int data, int addr, rel_time_t time )
{
	MEM_ACCESS( time, addr )
	
	// RAM
	pipeline_store(unsigned(addr),unsigned(data));
	int reg = addr - 0xF0;
	if ( reg >= 0 ) // 64%
	{
		// $F0-$FF
		if ( reg < reg_count ) // 87%
]=])

starfox_spc_pipeline_replace([=[

//// Run

// Prefix and suffix for CPU emulator function
#define SPC_CPU_RUN_FUNC \
BOOST::uint8_t* SNES_SPC::run_until_( time_t end_time )\
{\
	rel_time_t rel_time = m.spc_time - end_time;\
	assert( rel_time <= 0 );\
	m.spc_time = end_time;\
	m.dsp_time += rel_time;\
]=] [=[

//// Run

// Prefix and suffix for CPU emulator function
#define SPC_CPU_RUN_FUNC \
template<bool Predict> BOOST::uint8_t* SNES_SPC::run_until_cpu_( time_t end_time )\
{\
	rel_time_t rel_time = m.spc_time - end_time;\
	assert( rel_time <= 0 );\
	m.spc_time = end_time;\
	m.dsp_time += rel_time;\
]=])

starfox_spc_pipeline_replace([=[
		save_extra();
}

// Inclusion here allows static memory access functions and better optimization
#include "SPC_CPU.h"
]=] [=[
		save_extra();
}

// Inclusion here allows static memory access functions and better optimization
#include "SPC_CPU.h"

BOOST::uint8_t* SNES_SPC::run_until_(time_t end_time) {
    return pipeline_enabled_?run_until_cpu_<true>(end_time):run_until_cpu_<false>(end_time);
}
]=])

starfox_write_spc_file("SNES_SPC.cpp" "${pipeline_source}")

file(READ "${STARFOX_SPC_SOURCE}/SPC_CPU.h" pipeline_source)

starfox_spc_pipeline_replace([=[
#else
	#define SUSPICIOUS_OPCODE( name ) dprintf( "SPC: suspicious opcode: " name "\n" )
#endif

#define CPU_READ( time, offset, addr )\
	cpu_read( addr, time + offset )

#define CPU_WRITE( time, offset, addr, data )\
	cpu_write( data, addr, time + offset )

#if SPC_MORE_ACCURACY
	#define CPU_READ_TIMER( time, offset, addr, out )\
		{ out = CPU_READ( time, offset, addr ); }

]=] [=[
#else
	#define SUSPICIOUS_OPCODE( name ) dprintf( "SPC: suspicious opcode: " name "\n" )
#endif

#define CPU_READ( time, offset, addr )\
	pipeline_cpu_read<Predict>( addr, time + offset )

#define CPU_WRITE( time, offset, addr, data )\
	pipeline_cpu_write<Predict>( data, addr, time + offset )

#if SPC_MORE_ACCURACY
	#define CPU_READ_TIMER( time, offset, addr, out )\
		{ out = CPU_READ( time, offset, addr ); }

]=])

starfox_spc_pipeline_replace([=[
			out = t->counter;\
			t->counter = 0;\
		}\
		else\
		{\
			out = ram [dp_addr];\
			int i = dp_addr - 0xF0;\
			if ( (unsigned) i < 0x10 )\
				out = cpu_read_smp_reg( i, adj_time );\
		}\
	}
]=] [=[
			out = t->counter;\
			t->counter = 0;\
		}\
		else\
		{\
			pipeline_touch_cpu<Predict>(unsigned(dp_addr));\
            out = ram [dp_addr];\
			int i = dp_addr - 0xF0;\
			if ( (unsigned) i < 0x10 )\
				out = cpu_read_smp_reg( i, adj_time );\
		}\
	}
]=])

starfox_spc_pipeline_replace([=[

#define READ_DP_TIMER(  time, addr, out )   CPU_READ_TIMER( rel_time, TIME_ADJ(time), DP_ADDR( addr ), out )
#define READ_DP(  time, addr )              READ ( time, DP_ADDR( addr ) )
#define WRITE_DP( time, addr, data )        WRITE( time, DP_ADDR( addr ), data )

#define READ_PROG16( addr )                 GET_LE16( ram + (addr) )

#define SET_PC( n )     (pc = ram + (n))
#define GET_PC()        (pc - ram)
#define READ_PC( pc )   (*(pc))
#define READ_PC16( pc ) GET_LE16( pc )

// TODO: remove non-wrapping versions?
#define SPC_NO_SP_WRAPAROUND 0

#define SET_SP( v )     (sp = ram + 0x101 + (v))
]=] [=[

#define READ_DP_TIMER(  time, addr, out )   CPU_READ_TIMER( rel_time, TIME_ADJ(time), DP_ADDR( addr ), out )
#define READ_DP(  time, addr )              READ ( time, DP_ADDR( addr ) )
#define WRITE_DP( time, addr, data )        WRITE( time, DP_ADDR( addr ), data )

#define READ_PROG16( addr ) pipeline_read_code_word<Predict>(ram+(addr))

#define SET_PC( n )     (pc = ram + (n))
#define GET_PC()        (pc - ram)
#define READ_PC( pc ) pipeline_read_code<Predict>(pc)
#define READ_PC16( pc ) pipeline_read_code_word<Predict>(pc)

// TODO: remove non-wrapping versions?
#define SPC_NO_SP_WRAPAROUND 0

#define SET_SP( v )     (sp = ram + 0x101 + (v))
]=])

starfox_spc_pipeline_replace([=[
#define PUSH16( data )\
{\
	int addr = (sp -= 2) - ram;\
	if ( addr > 0x100 )\
	{\
		SET_LE16( sp, data );\
	}\
	else\
	{\
		ram [(uint8_t) addr + 0x100] = (uint8_t) data;\
		sp [1] = (uint8_t) (data >> 8);\
		sp += 0x100;\
	}\
}

#define PUSH( data )\
{\
	*--sp = (uint8_t) (data);\
	if ( sp - ram == 0x100 )\
		sp += 0x100;\
}

#define POP( out )\
]=] [=[
#define PUSH16( data )\
{\
	int addr = (sp -= 2) - ram;\
	if ( addr > 0x100 )\
	{\
		pipeline_store_word_cpu<Predict>(sp,unsigned(data));\
	}\
	else\
	{\
		pipeline_store_cpu<Predict>((uint8_t)addr+0x100U,unsigned(data));\
		pipeline_store_cpu<Predict>(unsigned(sp+1-ram),unsigned(data)>>8U);\
		sp += 0x100;\
	}\
}

#define PUSH( data )\
{\
	--sp; pipeline_store_cpu<Predict>(unsigned(sp-ram),unsigned(data));\
	if ( sp - ram == 0x100 )\
		sp += 0x100;\
}

#define POP( out )\
]=])

starfox_spc_pipeline_replace([=[
	}\
}

#endif

#define MEM_BIT( rel ) CPU_mem_bit( pc, rel_time + rel )

unsigned SNES_SPC::CPU_mem_bit( uint8_t const* pc, rel_time_t rel_time )
{
	unsigned addr = READ_PC16( pc );
	unsigned t = READ( 0, addr & 0x1FFF ) >> (addr >> 13);
	return t << 8 & 0x100;
}
]=] [=[
	}\
}

#endif

#define MEM_BIT( rel ) CPU_mem_bit<Predict>( pc, rel_time + rel )

template<bool Predict> unsigned SNES_SPC::CPU_mem_bit( uint8_t const* pc, rel_time_t rel_time )
{
	unsigned addr = READ_PC16( pc );
	unsigned t = READ( 0, addr & 0x1FFF ) >> (addr >> 13);
	return t << 8 & 0x100;
}
]=])

starfox_spc_pipeline_replace([=[
	
	check( (unsigned) a < 0x100 );
	check( (unsigned) x < 0x100 );
	check( (unsigned) y < 0x100 );
	
    #if !SPC_MORE_ACCURACY && defined(NDEBUG)
    // No I/O, ROM writes, self-modification or event boundary may enter here.
    if (pc == native_entry && rel_time <= -53
        && pc[0]==0xaa && pc[3]==0x8a && pc[6]==0x2b && pc[8]==0x2b
        && pc[10]==0xe4 && pc[12]==0x25 && pc[15]==0x08 && pc[17]==0xd6
]=] [=[
	
	check( (unsigned) a < 0x100 );
	check( (unsigned) x < 0x100 );
	check( (unsigned) y < 0x100 );
	
    pipeline_touch_cpu<Predict>(unsigned(pc-ram));
    pipeline_touch_cpu<Predict>(unsigned(pc-ram)+2U);
    #if !SPC_MORE_ACCURACY && defined(NDEBUG)
    // No I/O, ROM writes, self-modification or event boundary may enter here.
    if (pc == native_entry && rel_time <= -53
        && pc[0]==0xaa && pc[3]==0x8a && pc[6]==0x2b && pc[8]==0x2b
        && pc[10]==0xe4 && pc[12]==0x25 && pc[15]==0x08 && pc[17]==0xd6
]=])

starfox_spc_pipeline_replace([=[
            // guard, including aliases that can mutate later code operands.
            unsigned batch = unsigned(base0-0x200)<0xfdc0-255
                && unsigned(base1-0x200)<0xfdc0-255
                && (base0+255<code_addr || base0>=code_addr+32)
                && (base1+255<code_addr || base1>=code_addr+32) ? 256 : 1;
            unsigned iterations=0;
            do {
            c=(((ram[bit0&0x1fff]>>(bit0>>13))
                ^ (ram[bit1&0x1fff]>>(bit1>>13)))<<8)&0x100;
            unsigned rotate0=ram[dp+pc[7]]<<1;
            ram[dp+pc[7]]=uint8_t(rotate0|(c>>8&1));c=rotate0;
            unsigned rotate1=ram[dp+pc[9]]<<1;
            ram[dp+pc[9]]=uint8_t(rotate1|(c>>8&1));c=rotate1;
            a=(ram[dp+pc[11]]&ram[and_addr])|pc[16];
            ram[base0+y]=uint8_t(a);y=uint8_t(y+1);
            a=ram[dp+pc[22]]|pc[24];
            ram[base1+y]=uint8_t(a);y=uint8_t(y+1);
            nz=x-1;x=uint8_t(nz);
            rel_time+=53;
            ++iterations;
            if (!x) { rel_time-=2;pc+=32;break; }
            } while(iterations<batch && rel_time<=-53);
]=] [=[
            // guard, including aliases that can mutate later code operands.
            unsigned batch = unsigned(base0-0x200)<0xfdc0-255
                && unsigned(base1-0x200)<0xfdc0-255
                && (base0+255<code_addr || base0>=code_addr+32)
                && (base1+255<code_addr || base1>=code_addr+32) ? 256 : 1;
            if constexpr(Predict) {
                pipeline_touch_cpu<Predict>(unsigned(pc-ram)+31U);
                pipeline_touch_cpu<Predict>(bit0&0x1fffU);pipeline_touch_cpu<Predict>(bit1&0x1fffU);pipeline_touch_cpu<Predict>(and_addr);
                pipeline_touch_cpu<Predict>(dp+pc[7]);pipeline_touch_cpu<Predict>(dp+pc[9]);
                pipeline_touch_cpu<Predict>(dp+pc[11]);pipeline_touch_cpu<Predict>(dp+pc[22]);
                pipeline_touch_cpu<Predict>(base0);pipeline_touch_cpu<Predict>(base0+255U);
                pipeline_touch_cpu<Predict>(base1);pipeline_touch_cpu<Predict>(base1+255U);
            }
            unsigned iterations=0;
            do {
            c=(((ram[bit0&0x1fff]>>(bit0>>13))
                ^ (ram[bit1&0x1fff]>>(bit1>>13)))<<8)&0x100;
            unsigned rotate0=ram[dp+pc[7]]<<1;
            pipeline_store_cpu<Predict>(dp+pc[7],rotate0|(c>>8&1));c=rotate0;
            unsigned rotate1=ram[dp+pc[9]]<<1;
            pipeline_store_cpu<Predict>(dp+pc[9],rotate1|(c>>8&1));c=rotate1;
            a=(ram[dp+pc[11]]&ram[and_addr])|pc[16];
            pipeline_store_cpu<Predict>(base0+y,unsigned(a));y=uint8_t(y+1);
            a=ram[dp+pc[22]]|pc[24];
            pipeline_store_cpu<Predict>(base1+y,unsigned(a));y=uint8_t(y+1);
            nz=x-1;x=uint8_t(nz);
            rel_time+=53;
            ++iterations;
            if (!x) { rel_time-=2;pc+=32;break; }
            } while(iterations<batch && rel_time<=-53);
]=])

starfox_spc_pipeline_replace([=[
    // A DSP catch-up may write echo RAM, so recheck the final branch afterwards.
    static uint8_t const dsp_copy_code[16]={0xf6,0x9b,0x0e,0xc5,0xf2,0x00,
        0xf6,0xa5,0x0e,0x5d,0xe6,0xc5,0xf3,0x00,0xfe,0xe2};
    if(native_blocks_ && pc==ram+0x461 && rel_time<=-31
        && !memcmp(pc,dsp_copy_code,16)) {
        rel_time+=5; a=nz=READ(0,0x0e9b+y);
        rel_time+=5; WRITE(0,0xf2,a);
        rel_time+=5; a=nz=READ(0,0x0ea5+y);
        rel_time+=2; x=nz=a;
        rel_time+=3; a=nz=READ(0,dp+x);
]=] [=[
    // A DSP catch-up may write echo RAM, so recheck the final branch afterwards.
    static uint8_t const dsp_copy_code[16]={0xf6,0x9b,0x0e,0xc5,0xf2,0x00,
        0xf6,0xa5,0x0e,0x5d,0xe6,0xc5,0xf3,0x00,0xfe,0xe2};
    if(native_blocks_ && pc==ram+0x461 && rel_time<=-31
        && !memcmp(pc,dsp_copy_code,16)) {
        pipeline_touch_cpu<Predict>(unsigned(pc-ram)+15U);
        rel_time+=5; a=nz=READ(0,0x0e9b+y);
        rel_time+=5; WRITE(0,0xf2,a);
        rel_time+=5; a=nz=READ(0,0x0ea5+y);
        rel_time+=2; x=nz=a;
        rel_time+=3; a=nz=READ(0,dp+x);
]=])

starfox_spc_pipeline_replace([=[
		pc += 2;
		
		#if !SPC_MORE_ACCURACY
		{
			int i = dp + temp;
			ram [i] = (uint8_t) data;
			i -= 0xF0;
			if ( (unsigned) i < 0x10 ) // 76%
			{
				REGS [i] = (uint8_t) data;
				
]=] [=[
		pc += 2;
		
		#if !SPC_MORE_ACCURACY
		{
			int i = dp + temp;
			pipeline_touch_cpu<Predict>(unsigned(i));
            pipeline_store_cpu<Predict>(unsigned(i),unsigned(data));
			i -= 0xF0;
			if ( (unsigned) i < 0x10 ) // 76%
			{
				REGS [i] = (uint8_t) data;
				
]=])

starfox_spc_pipeline_replace([=[
	case 0xC4: // MOV dp,a
		++pc;
		#if !SPC_MORE_ACCURACY
		{
			int i = dp + data;
			ram [i] = (uint8_t) a;
			i -= 0xF0;
			if ( (unsigned) i < 0x10 ) // 39%
			{
				unsigned sel = i - 2;
				REGS [i] = (uint8_t) a;
]=] [=[
	case 0xC4: // MOV dp,a
		++pc;
		#if !SPC_MORE_ACCURACY
		{
			int i = dp + data;
			pipeline_touch_cpu<Predict>(unsigned(i));
            pipeline_store_cpu<Predict>(unsigned(i),unsigned(a));
			i -= 0xF0;
			if ( (unsigned) i < 0x10 ) // 39%
			{
				unsigned sel = i - 2;
				REGS [i] = (uint8_t) a;
]=])

starfox_write_spc_file("SPC_CPU.h" "${pipeline_source}")
