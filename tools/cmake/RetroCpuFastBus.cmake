# Build a private copy of the pinned MIT RetroCPU sources. Plain RAM and ROM
# pages resolve inline in SystemBus::ReadByte/WriteByte; I/O, unmapped and
# read-only pages keep the upstream code under a new name. Every emulated
# value, open-bus byte and cycle count is unchanged.
set(STARFOX_RETRO_CPU_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/retro_cpu")
file(MAKE_DIRECTORY "${STARFOX_RETRO_CPU_SOURCE}/cpu/65816")
foreach(name IN ITEMS host_system.h jit.h LICENSE.txt)
    configure_file("${retro_cpu_SOURCE_DIR}/${name}"
        "${STARFOX_RETRO_CPU_SOURCE}/${name}" COPYONLY)
endforeach()
file(GLOB retro_cpu_65816_files "${retro_cpu_SOURCE_DIR}/cpu/65816/*")
foreach(source IN LISTS retro_cpu_65816_files)
    get_filename_component(name "${source}" NAME)
    # cpu_65c816.h is written patched below.
    if(NOT IS_DIRECTORY "${source}" AND NOT name STREQUAL "cpu_65c816.h")
        configure_file("${source}"
            "${STARFOX_RETRO_CPU_SOURCE}/cpu/65816/${name}" COPYONLY)
    endif()
endforeach()
function(starfox_write_retro_cpu_file name contents)
    set(destination "${STARFOX_RETRO_CPU_SOURCE}/${name}")
    if(EXISTS "${destination}")
        file(READ "${destination}" previous)
        if(previous STREQUAL contents)
            return()
        endif()
    endif()
    file(WRITE "${destination}" "${contents}")
endfunction()
# Replace one anchor that must occur exactly once in the pinned source.
function(starfox_replace_retro_cpu_once variable anchor replacement)
    string(FIND "${${variable}}" "${anchor}" first)
    string(FIND "${${variable}}" "${anchor}" last REVERSE)
    if(first EQUAL -1 OR NOT first EQUAL last)
        message(FATAL_ERROR "Pinned RetroCPU bus needs review: ${anchor}")
    endif()
    string(REPLACE "${anchor}" "${replacement}" result "${${variable}}")
    set(${variable} "${result}" PARENT_SCOPE)
endfunction()

file(READ "${retro_cpu_SOURCE_DIR}/cpu.h" bus_header)
starfox_replace_retro_cpu_once(bus_header
    "uint32_t ReadByte(cpuaddr_t addr, uint8_t *data);" [=[
// devkitARM's GCC keeps a plain inline definition out of line (3,420 call
// sites); the fast path only pays off when it is forced into the handlers.
#if defined(__GNUC__) || defined(__clang__)
#define STARFOX_BUS_LIKELY(condition) __builtin_expect(!!(condition), 1)
#define STARFOX_BUS_INLINE inline __attribute__((always_inline))
#else
#define STARFOX_BUS_LIKELY(condition) (condition)
#define STARFOX_BUS_INLINE inline
#endif
	uint32_t ReadByteSlow(cpuaddr_t addr, uint8_t *data);
	uint32_t WriteByteSlow(cpuaddr_t addr, uint8_t v);
	STARFOX_BUS_INLINE uint32_t ReadByte(cpuaddr_t addr, uint8_t *data)
	{
		const cpuaddr_t masked = addr & mem_mask;
		const Page& p = memory.pages[masked >> memory.page_shift];
		if(STARFOX_BUS_LIKELY(open_bus_is_data && p.ptr
			&& (p.io_mask & masked) != p.io_eq)) {
			const uint8_t v = p.ptr[masked & memory.page_mask];
			open_bus = v;
			*data = v;
			return p.cycles_per_access;
		}
		return ReadByteSlow(addr, data);
	}]=])
starfox_replace_retro_cpu_once(bus_header
    "uint32_t WriteByte(cpuaddr_t addr, uint8_t v);" [=[
STARFOX_BUS_INLINE uint32_t WriteByte(cpuaddr_t addr, uint8_t v)
	{
		const cpuaddr_t masked = addr & mem_mask;
#ifdef STARFOX_VERIFY_FAST_65816
		// Verification builds list the stores of each interpreted instruction.
		{
			extern uint32_t starfox_bus_write_count;
			extern uint32_t starfox_bus_write_address[16];
			extern uint8_t starfox_bus_write_value[16];
			if(starfox_bus_write_count < 16U) {
				starfox_bus_write_address[starfox_bus_write_count] = masked;
				starfox_bus_write_value[starfox_bus_write_count] = v;
			}
			++starfox_bus_write_count;
		}
#endif
		Page& p = memory.pages[masked >> memory.page_shift];
		if(STARFOX_BUS_LIKELY(open_bus_is_data && p.ptr
			&& !(p.flags & Page::kReadOnly)
			&& (p.io_mask & masked) != p.io_eq)) {
			p.ptr[masked & memory.page_mask] = v;
			open_bus = v;
			return p.cycles_per_access;
		}
		return WriteByteSlow(addr, v);
	}]=])
starfox_write_retro_cpu_file("cpu.h" "${bus_header}")

file(READ "${retro_cpu_SOURCE_DIR}/cpu.cc" bus_source)
starfox_replace_retro_cpu_once(bus_source
    "uint32_t SystemBus::ReadByte(cpuaddr_t addr, uint8_t *data)"
    "uint32_t SystemBus::ReadByteSlow(cpuaddr_t addr, uint8_t *data)")
starfox_replace_retro_cpu_once(bus_source
    "uint32_t SystemBus::WriteByte(uint32_t addr, uint8_t v)"
    "uint32_t SystemBus::WriteByteSlow(uint32_t addr, uint8_t v)")
starfox_write_retro_cpu_file("cpu.cc" "${bus_source}")

# The CPU's own accessors wrap every bus access. devkitARM left them out of
# line (1,977 ReadU8 and 377 WriteU8 call sites with the bus above), so each
# emulated memory access paid a call and return around the inline bus path.
file(READ "${retro_cpu_SOURCE_DIR}/cpu/65816/cpu_65c816.h" cpu_header)
foreach(accessor IN ITEMS
        "void ReadU8(uint32_t addr, uint8_t& v)"
        "void WriteU8(uint32_t addr, uint8_t v)"
        "void ReadU16NoCrossBank(uint32_t base, uint32_t addr, uint16_t& v)"
        "uint16_t PeekU16(uint32_t addr)")
    starfox_replace_retro_cpu_once(cpu_header "${accessor}"
        "STARFOX_BUS_INLINE ${accessor}")
endforeach()
starfox_write_retro_cpu_file("cpu/65816/cpu_65c816.h" "${cpu_header}")
