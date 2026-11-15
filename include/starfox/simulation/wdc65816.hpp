#pragma once

#include "starfox/assets/rom.hpp"
#include "starfox/simulation/snes_ppu.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The 65C816 machine that runs the cartridge code: the pinned RetroCPU core
// behind the SNES memory map and the PPU, APU, MSU-1 and DMA registers the game
// uses, native translations of the Super FX routines it launches, and bounded
// call and task loops. MapVm owns one; host tests also build their own.

namespace starfox::simulation {

// A call or task left mapped ROM or used up its instruction budget.
// entry_address is the routine entered, program_address where execution
// stood, instructions the count it executed.
class Wdc65816ExecutionError : public std::runtime_error {
public:
    Wdc65816ExecutionError(std::string message,
        std::uint32_t entry_address,
        std::uint32_t program_address,
        std::size_t instructions)
        : std::runtime_error{std::move(message)},
          entry_address_(entry_address),
          program_address_(program_address),
          instructions_(instructions) {}

    [[nodiscard]] std::uint32_t entry_address() const noexcept {
        return entry_address_;
    }
    [[nodiscard]] std::uint32_t program_address() const noexcept {
        return program_address_;
    }
    [[nodiscard]] std::size_t instructions() const noexcept {
        return instructions_;
    }

private:
    std::uint32_t entry_address_{};
    std::uint32_t program_address_{};
    std::size_t instructions_{};
};

// The registers a call or task starts with; on return, the ones the routine
// left. `stack` is S, `direct` D, `data_bank` DBR and `status` P (callers of
// cartridge routines mostly pass $24: 8-bit A, 16-bit X/Y, IRQ disabled).
struct Wdc65816Registers {
    std::uint16_t a{};
    std::uint16_t x{};
    std::uint16_t y{};
    std::uint16_t direct{};
    std::uint16_t stack{0x1ff};
    std::uint8_t data_bank{};
    // Native mode, 16-bit accumulator and index registers, IRQ disabled.
    std::uint8_t status{0x04};
};

// A CPU write to APU port $2140 + `port`, stamped with the SPC700 clock offset
// set by set_apu_clock_offset().
struct ApuPortWrite {
    std::uint8_t port{};
    std::uint8_t value{};
    std::uint32_t clock_offset{};

    friend bool operator==(const ApuPortWrite&, const ApuPortWrite&) = default;
};

// A CPU write to MSU-1 register `address` ($2004-$2007), stamped likewise.
struct MsuRegisterWrite {
    std::uint16_t address{};
    std::uint8_t value{};
    std::uint32_t clock_offset{};

    friend bool operator==(const MsuRegisterWrite&, const MsuRegisterWrite&)
        = default;
};

// One begin or resume of a task: the instructions it ran, and either the
// stop address it paused in front of or `returned` when it reached its end.
struct Wdc65816TaskResult {
    std::size_t instructions{};
    std::uint32_t stop_address{};
    bool returned{};
};

// Snapshot of CONTINUE.ASM's dedicated MSHOWOBJ3 launch. Unlike ordinary
// gameplay objects this model is drawn directly into FOXY_CONTINUE's 224x192
// bitmap and therefore has no entry in the host ObjectPool.
struct NativeModelDrawState {
    bool active{};
    std::uint16_t shape{};
    std::int16_t x{};
    std::int16_t y{};
    std::int16_t z{};
    std::uint16_t rotation_x{};
    std::uint16_t rotation_y{};
    std::uint16_t rotation_z{};
    std::int16_t vanish_x{112};
    std::int16_t vanish_y{96};
    std::uint16_t animation_frame{};
    std::uint16_t colour_frame{};
    std::uint16_t colour_table{};
};

// Project-owned adapter around the pinned MIT RetroCPU core. It supplies the
// SNES LoROM/WRAM address map and bounded native-mode subroutine execution.
// Instructions run in the compact interpreter (wdc65816_lean.cpp) and its
// native kernels where they apply, otherwise in the library's interpreter;
// each of these paths leaves registers, flags, cycles, memory and open bus
// exactly as the library's interpreter would.
class Wdc65816 {
public:
    // Star Fox EX's battery RAM, mapped at $71:0000.
    static constexpr std::size_t cartridge_ram_size = 0x10000U;

    // `symbols` (may be null) names the routines and variables the adapter
    // services; a missing symbol leaves the feature that needs it inactive.
    // Keeps pointers to `rom` and `symbols`, which must outlive it.
    explicit Wdc65816(
        const assets::RomImage& rom,
        const assets::SymbolMap* symbols = nullptr);
    ~Wdc65816();
    Wdc65816(Wdc65816&&) noexcept;
    Wdc65816& operator=(Wdc65816&&) noexcept;
    Wdc65816(const Wdc65816&) = delete;
    Wdc65816& operator=(const Wdc65816&) = delete;

    // CPU registers and modes, WRAM, Super FX RAM, cartridge RAM, PPU state,
    // I/O latches and the logged APU/MSU writes (schema 0x43505501).
    [[nodiscard]] std::vector<std::uint8_t> save_state() const;
    // Throws std::runtime_error for a damaged or foreign archive; this
    // machine is unchanged then.
    void load_state(std::span<const std::uint8_t> bytes);

    // Bus accesses: they update the open-bus byte and run I/O side effects,
    // as cartridge code would see them.
    [[nodiscard]] std::uint8_t read8(std::uint32_t address) const;
    [[nodiscard]] std::uint16_t read16(std::uint32_t address) const;
    // Presentation/debug RAM reads never touch open bus or I/O latches.
    // Unsupported/ROM/I/O addresses return nullopt rather than reading them.
    [[nodiscard]] std::optional<std::uint8_t> peek_ram8(std::uint32_t address) const noexcept;
    [[nodiscard]] std::optional<std::uint16_t> peek_ram16(std::uint32_t address) const noexcept;
    // Bus writes, like read8(); a store into the watched block leaves a mark.
    void write8(std::uint32_t address, std::uint8_t value);
    void write16(std::uint32_t address, std::uint16_t value);
    // Copy a contiguous block only when every byte maps to the same WRAM
    // region. The final byte updates open bus exactly as ordinary bus reads
    // or writes would. False leaves both memory and bus state unchanged.
    [[nodiscard]] bool read_mapped_wram_block(
        std::uint32_t address, std::span<std::uint8_t> output) const noexcept;
    [[nodiscard]] bool write_mapped_wram_block(
        std::uint32_t address, std::span<const std::uint8_t> input) noexcept;
    // The bytes of a block that maps entirely to one WRAM region, or null.
    // Looking at them is not a bus access: open bus keeps its value.
    [[nodiscard]] const std::uint8_t* mapped_wram(
        std::uint32_t address, std::size_t length) const noexcept;
    // Change marks for a block of work RAM (the object tables), so that its
    // mirror need not compare all of it. Stores leave a mark on the 64-byte
    // chunk they land in; a writer that cannot leave marks voids them all
    // until the next clear.
    //
    // Whether these bytes lie inside the block that is marked.
    [[nodiscard]] bool wram_block_watched(std::uint32_t address, std::size_t length) const noexcept;
    // True when every store to these bytes since clear_wram_marks() left one.
    [[nodiscard]] bool wram_marks_cover(std::uint32_t address, std::size_t length) const noexcept;
    // The caller compared everything and found it as it was at the last
    // clear: the marks that were there before a writer voided them stand.
    void revalidate_wram_marks() const noexcept;
    // Whether a mark reaches these bytes. Needs wram_marks_cover().
    [[nodiscard]] bool wram_marked(std::uint32_t address, std::size_t length) const noexcept;
    // Whether the marked parts of these bytes still equal `reference`, a copy
    // of them as they were at the last clear. Needs wram_marks_cover().
    [[nodiscard]] bool wram_marked_equal(
        std::uint32_t address, std::span<const std::uint8_t> reference) const noexcept;
    // Brings such a copy up to date. Needs wram_marks_cover().
    void wram_copy_marked(std::uint32_t address, std::span<std::uint8_t> reference) const noexcept;
    void clear_wram_marks() noexcept;
    // A checkpoint keeps the previous contents of whatever is stored to after
    // it, so that read_wram_checkpoint() can give the block back as it was
    // when the checkpoint opened. False when this processor cannot keep one;
    // the caller then copies what it needs. While one is open,
    // clear_wram_marks() does nothing.
    [[nodiscard]] bool open_wram_checkpoint() noexcept;
    void close_wram_checkpoint() noexcept;
    // False when no checkpoint is open or the bytes lie outside the block.
    [[nodiscard]] bool read_wram_checkpoint(
        std::uint32_t address, std::span<std::uint8_t> output) const noexcept;
    // False, copying nothing, unless `bytes` is cartridge_ram_size long.
    [[nodiscard]] bool load_cartridge_ram(
        std::span<const std::uint8_t> bytes) noexcept;
    [[nodiscard]] std::span<const std::uint8_t> cartridge_ram() const noexcept;
    // Move out the writes logged since the previous take.
    [[nodiscard]] std::vector<ApuPortWrite> take_apu_port_writes();
    [[nodiscard]] std::vector<MsuRegisterWrite> take_msu_register_writes();
    // SPC-only frontends must still consume the cartridge's MSU event log.
    // Keep the largest packet allocation for reuse without changing registers.
    void discard_msu_register_writes() noexcept;
    // SPC700 clocks since the start of the current source update, stamped on
    // the writes that follow.
    void set_apu_clock_offset(std::uint32_t clocks) noexcept;
    // The SPC700's output latch, as later reads of $2140-$2143 see it. Until
    // the first call, and during an IPL upload, reads return the CPU's own
    // writes (the boot ROM echo).
    void set_apu_output_ports(
        const std::array<std::uint8_t, 4>& ports) noexcept;
    // One-shot main-thread dependency: output reads and IPL latch changes
    // need prior audio. Callbacks and their context are never serialized.
    // Connected driver writes merely queue input and do not invoke the fence.
    using ApuAccessFence = void (*)(void*) noexcept;
    void set_apu_access_fence(ApuAccessFence fence, void* context) noexcept;
    [[nodiscard]] const SnesPpuState& ppu_state() const noexcept;
    // Changes whenever PPU VRAM, CGRAM, or OAM is written. This lets the
    // renderer validate a cached foreground without comparing 65 KiB of VRAM
    // on every duplicate presentation.
    [[nodiscard]] std::uint64_t ppu_memory_revision() const noexcept;
    // Changes whenever VRAM is written. Both revisions start from a value
    // unique to each machine, including one restored by load_state().
    [[nodiscard]] std::uint64_t ppu_vram_revision() const noexcept;
    // The last MSHOWOBJ3 launch (see NativeModelDrawState).
    [[nodiscard]] const NativeModelDrawState& native_model_draw() const noexcept;
    void set_native_model_draw(const NativeModelDrawState& state) noexcept;
    // Runs and instructions of the object and sprite kernels, then of the
    // pair kernel; diagnostics, never serialized.
    [[nodiscard]] std::array<std::uint64_t,4> native_execution_counts() const noexcept;
    // Compiled calls/instructions, interpreted instructions, missing entries,
    // rejected code bytes and short budgets; diagnostics, never serialized.
    [[nodiscard]] std::array<std::uint64_t,6> native_dispatch_counts() const noexcept;
    // Super FX program addresses launched that have no native translation,
    // each listed once; saved with the state.
    [[nodiscard]] const std::vector<std::uint32_t>& unknown_superfx_launches()
        const noexcept;
    // Counts the SPC700 uploads the cartridge starts ($ff written to $2140
    // while the driver is idle).
    [[nodiscard]] std::uint64_t apu_upload_generation() const noexcept;
    // Host writes to PPU memory, from CGRAM colour index `first_colour`
    // (colours past 255 are dropped) or VRAM byte `byte_offset` (wrapping at
    // 64 KiB). The revisions advance only when a byte changes.
    void write_cgram(
        std::uint16_t first_colour,
        std::span<const std::uint16_t> colours) noexcept;
    void write_vram(
        std::uint16_t byte_offset,
        std::span<const std::uint8_t> bytes) noexcept;
    // Copies min(length, 544) bytes from bus address `source` into OAM.
    void upload_oam(std::uint32_t source, std::size_t length);
    // Clears BITMAP1 in Super FX RAM for a new frame, unless the low byte of
    // M_CLRBITMAPS is zero.
    void begin_superfx_bitmap_frame();
    // Submit the source 224x192 Super FX bitmap through FOXIRQ's exact two
    // VRAM transfers and buffer swap. Native front-end text is CPU-drawn
    // into this bitmap even when model geometry is host-rendered.
    void submit_superfx_bitmap();
    void set_bg1_scroll(std::int16_t x, std::int16_t y) noexcept;
    void set_bg2_scroll(std::int16_t x, std::int16_t y) noexcept;
    // ENDSEQ's SEQSCROLL runs once per raster, independently of CPU tasks.
    void tick_ending_video_phase();
    // One raster of IRQ-side background work: SETBG2VOFS's requested scroll,
    // the BG3 scroll captured by TRANSFER_L, then the tunnel metadata below.
    void tick_background_video_phase();
    // Whether channel 1 (EX's channel 2 when `alternate`) was speaking at its
    // last MCOPYFACE/MCOPYFACE2 launch; empty before one.
    [[nodiscard]] std::optional<bool> dialogue_speaking(bool alternate) const noexcept;
    // Recomputes SnesPpuState's tunnel flags and BG2 scanline table from RAM
    // and ROM without bus effects; throws std::runtime_error when a table
    // address points at I/O or unmapped memory.
    void refresh_background_metadata();
    // Runs the native MDRAWSPHERE with M_SPRITE = `sprite` (the low seven
    // bits pick the texture).
    void draw_planet_sphere(std::uint16_t sprite);
    void set_bg2_vertical_offsets_enabled(bool enabled) noexcept;
    // When `enabled`, copies one BG2 horizontal offset per scanline from the
    // 3-byte HDMA records at work-RAM offset `source`.
    void capture_bg2_horizontal_offsets(
        std::uint16_t source, bool enabled) noexcept;

    // Runs a routine entered directly and expected to return with RTL.
    // Returns the number of executed instructions and writes back registers.
    // With service_transfer_flag, the transfer requests the routine raises
    // (TRANS_FLAG at $0000, the planet DMA flag) are completed between
    // instructions, as the NMI side would. Ends any task. Throws
    // Wdc65816ExecutionError when execution leaves mapped ROM or reaches
    // instruction_limit.
    std::size_t call_long(
        std::uint32_t address,
        Wdc65816Registers& registers,
        std::size_t instruction_limit = 1'000'000,
        bool service_transfer_flag = false);

    // Runs a same-bank routine entered directly and expected to return with
    // RTS. This is used for source-local screen helpers that were never given
    // a JSL/RTL wrapper.
    std::size_t call_near(
        std::uint32_t address,
        Wdc65816Registers& registers,
        std::size_t instruction_limit = 1'000'000,
        bool service_transfer_flag = false);

    // Starts a long-call routine whose source control flow spans multiple
    // presentation frames. Execution pauses before any stop address and can
    // later continue with resume_task(), preserving the complete CPU state.
    Wdc65816TaskResult begin_long_task(
        std::uint32_t address,
        Wdc65816Registers& registers,
        std::span<const std::uint32_t> stop_addresses,
        std::size_t instruction_limit = 1'000'000,
        bool service_transfer_flag = false);

    // Starts a same-bank RTS routine as a resumable task. This is the task
    // counterpart of call_near() and is used by source screen sequences such
    // as END_LEVEL_SEQ that yield once per TRANSFER_L call.
    Wdc65816TaskResult begin_near_task(
        std::uint32_t address,
        Wdc65816Registers& registers,
        std::span<const std::uint32_t> stop_addresses,
        std::size_t instruction_limit = 1'000'000,
        bool service_transfer_flag = false);

    // Continues the active task. The instruction at the address where the
    // previous call paused is executed before stop addresses are considered
    // again, allowing frame loops to use one stable source label as a yield.
    // Throws std::logic_error when no task is active; tasks fail as
    // call_long() does, which also ends them.
    Wdc65816TaskResult resume_task(
        Wdc65816Registers& registers,
        std::span<const std::uint32_t> stop_addresses,
        std::size_t instruction_limit = 1'000'000,
        bool service_transfer_flag = false);

private:
    friend class MapVm;
    // The open-bus byte. MapVm saves and restores it around host mirror work
    // that cartridge code must not observe.
    [[nodiscard]] std::uint8_t open_bus() const noexcept;
    void restore_open_bus(std::uint8_t value) noexcept;
    template<bool AccelerateTransferCall> std::size_t call_impl(
        std::uint32_t address, Wdc65816Registers& registers,
        std::size_t instruction_limit, bool service_transfer_flag, bool long_return);
    std::size_t call(
        std::uint32_t address,
        Wdc65816Registers& registers,
        std::size_t instruction_limit,
        bool service_transfer_flag,
        bool long_return);
    Wdc65816TaskResult run_task(
        Wdc65816Registers& registers,
        std::span<const std::uint32_t> stop_addresses,
        std::size_t instruction_limit,
        bool service_transfer_flag);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace starfox::simulation
