#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// The host copy of the cartridge's object list: its al_/alx_ records and the
// active and free lists. MapVm keeps it in step with 65C816 work RAM around
// cartridge calls; GameSimulation and the 3DS frontend read it.

namespace starfox::simulation {

// A 1-based slot number; 0 means no object.
using ObjectHandle = std::uint16_t;
// NUMBER_AL of the retail cartridge, and the largest NUMBER_AL supported
// (Star Fox EX's).
inline constexpr std::size_t kOriginalMaximumObjects = 70;
inline constexpr std::size_t kMaximumObjects = 80;
// Bytes kept per alx_ record: 56 for Star Fox EX, of which retail uses 54.
inline constexpr std::size_t kExtendedObjectBytes = 56;

// Which STRUCTS.INC record layout the cartridge uses. Star Fox EX replaces
// al_weapontype with al_weaponnum/openal and inserts alx_frame.
enum class ObjectMemoryLayout : std::uint8_t {
    original,
    starfox_ex,
};

// Semantic form of the original al_/alx_ blocks. Narrow fields intentionally
// retain the 65816 wrapping behavior expected by strategy code.
struct GameObject {
    [[nodiscard]] bool operator==(const GameObject&) const = default;
    std::uint16_t shape{};
    ObjectHandle attached{};
    std::uint8_t flags{};
    std::uint8_t type{};
    std::uint8_t count{};
    std::uint8_t count1{};
    std::int16_t world_x{};
    std::int16_t world_y{};
    std::int16_t world_z{};
    std::uint8_t rotation_x{};
    std::uint8_t rotation_y{};
    std::uint8_t rotation_z{};
    std::int8_t velocity{};
    std::uint32_t strategy_address{};
    ObjectHandle immune_object{};
    ObjectHandle collision_object{};
    std::array<std::uint8_t, 4> strategy_flags{};
    std::int8_t skid_y{};
    std::array<std::int8_t, 6> scratch_bytes{};
    std::array<std::int16_t, 2> scratch_words{};
    std::uint8_t health{};
    std::uint8_t attack_power{};
    std::uint8_t weapon_type{};
    std::uint8_t open_al{};
    std::uint8_t collision_count{};
    std::uint8_t collision_flags{};
    std::int16_t velocity_x{};
    std::int16_t velocity_y{};
    std::int16_t velocity_z{};
    std::uint8_t hit_flags{};
    std::uint8_t colour_frame{};
    std::uint8_t animation_frame{};
    std::uint8_t sound1{};
    std::uint8_t sound2{};
    std::uint16_t colour_table{};
    std::uint8_t texture_scroll_x{};
    std::uint8_t texture_scroll_y{};
    ObjectHandle fire_object{};
    std::uint8_t strategy_state{};
    // PALVAROFFSET encodes the original parallel alx_ block as 0x80 plus
    // this zero-based index. Keeping its complete byte image lets the path
    // and strategy VMs preserve fields that do not yet have semantic names.
    std::array<std::uint8_t, kExtendedObjectBytes> extended{};
};

// Slots 1..capacity, each on either the active or the free list, in the
// cartridge's list order.
class ObjectPool {
public:
    // Throws std::invalid_argument unless 1 <= capacity <= kMaximumObjects.
    explicit ObjectPool(
        std::size_t capacity = kOriginalMaximumObjects,
        ObjectMemoryLayout layout = ObjectMemoryLayout::original);

    // Frees every slot (free list in slot order) and zeroes every object.
    void reset() noexcept;
    [[nodiscard]] std::vector<std::uint8_t> save_state() const;
    // Decode and validate a temporary pool before changing this instance.
    // Throws std::runtime_error for another version, capacity or layout, or
    // inconsistent lists or generations.
    void load_state(std::span<const std::uint8_t> bytes);
    // Takes the first free slot, zeroed and with a new generation, and links
    // it after `previous` in the active list (first when 0). Returns 0 when
    // no slot is free or `previous` is not active.
    [[nodiscard]] ObjectHandle allocate_after(ObjectHandle previous = 0) noexcept;
    // Unlinks an active object, clears every attached, immune, collision and
    // fire reference to it, and puts its slot first on the free list. False
    // for a handle that is not active.
    [[nodiscard]] bool remove(ObjectHandle handle) noexcept;
    [[nodiscard]] bool is_active(ObjectHandle handle) const noexcept;
    // Distinguishes successive objects in one slot; 0 for an inactive handle.
    [[nodiscard]] std::uint64_t generation(ObjectHandle handle) const noexcept;
    // A reference that can change the object: it marks the object as touched.
    // Do not keep it across a call into the cartridge logic.
    [[nodiscard]] GameObject& at(ObjectHandle handle);
    [[nodiscard]] const GameObject& at(ObjectHandle handle) const;
    // The same object for reading, from a pool that is not const.
    [[nodiscard]] const GameObject& get(ObjectHandle handle) const { return at(handle); }
    // Whether the object may have changed since clear_touched(): every way of
    // changing one goes through this class. The bridge to the cartridge logic
    // clears the mark once its own copy of the object equals it.
    [[nodiscard]] bool touched(ObjectHandle handle) const noexcept { return touched_[handle]; }
    void clear_touched(ObjectHandle handle) noexcept { touched_[handle] = false; }
    // Advances whenever the active or the free list changes.
    [[nodiscard]] std::uint64_t list_epoch() const noexcept { return list_epoch_; }
    // The lists in order. copy_active_handles() reuses the output's storage.
    [[nodiscard]] std::vector<ObjectHandle> active_handles() const;
    [[nodiscard]] std::vector<ObjectHandle> free_handles() const;
    void copy_active_handles(std::vector<ObjectHandle>& output) const;
    // Whether the two lists are exactly these sequences.
    [[nodiscard]] bool lists_match(
        const std::vector<ObjectHandle>& active,
        const std::vector<ObjectHandle>& free) const noexcept;
    // Relinks both lists to these sequences, which together must hold each
    // slot once (std::invalid_argument otherwise). A slot that becomes active
    // gets a new generation; one that becomes free is zeroed.
    void restore_lists(
        const std::vector<ObjectHandle>& active,
        const std::vector<ObjectHandle>& free);
    // List walks; 0 ends a list and is returned for an invalid handle.
    [[nodiscard]] ObjectHandle first_active() const noexcept { return first_active_; }
    [[nodiscard]] ObjectHandle next_active(ObjectHandle handle) const noexcept;
    [[nodiscard]] ObjectHandle first_free() const noexcept { return first_free_; }
    [[nodiscard]] ObjectHandle next_free(ObjectHandle handle) const noexcept;
    [[nodiscard]] std::size_t active_count() const noexcept { return active_count_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    // Byte `offset` of the object's al_ record, through the semantic fields.
    // Offsets run from 4 to AL_SIZE - 1 (55 retail, 56 EX), little-endian;
    // others and inactive handles throw std::out_of_range.
    [[nodiscard]] std::uint8_t read_base_byte(ObjectHandle handle, std::uint16_t offset) const;
    // Pack or import the contiguous AL_SIZE payload starting at byte 4.
    // Link words at 0..3 belong to the native list bridge.
    // The payload is 52 bytes (retail) or 53 (EX); std::invalid_argument
    // otherwise.
    void pack_base_payload(ObjectHandle handle, std::span<std::uint8_t> payload) const;
    void unpack_base_payload(ObjectHandle handle, std::span<const std::uint8_t> payload);
    [[nodiscard]] std::uint16_t read_base_word(ObjectHandle handle, std::uint16_t offset) const;
    void write_base_byte(ObjectHandle handle, std::uint16_t offset, std::uint8_t value);
    void write_base_word(ObjectHandle handle, std::uint16_t offset, std::uint16_t value);
    void write_base_long(ObjectHandle handle, std::uint16_t offset, std::uint32_t value);
    // PATH and map operands: an offset below $80 addresses the al_ record as
    // above, $80 + n byte n of the alx_ record. Writing an alx_ byte also
    // updates the GameObject field that mirrors it (strategy_state,
    // fire_object, colour and animation frames, sounds, colour table, texture
    // scroll). Throws std::out_of_range for an al_ offset outside the record
    // or n >= kExtendedObjectBytes.
    [[nodiscard]] std::uint8_t read_path_byte(ObjectHandle handle, std::uint8_t encoded_offset) const;
    [[nodiscard]] std::uint16_t read_path_word(ObjectHandle handle, std::uint8_t encoded_offset) const;
    void write_path_byte(ObjectHandle handle, std::uint8_t encoded_offset, std::uint8_t value);
    void write_path_word(ObjectHandle handle, std::uint8_t encoded_offset, std::uint16_t value);

private:
    struct Slot {
        GameObject object;
        ObjectHandle next{};
        ObjectHandle previous{};
        bool active{};
    };

    [[nodiscard]] bool valid_handle(ObjectHandle handle) const noexcept;

    std::array<Slot, kMaximumObjects + 1> slots_{};
    std::array<ObjectHandle, kMaximumObjects + 1> free_next_{};
    std::array<std::uint64_t, kMaximumObjects + 1> generations_{};
    // Not part of the saved state: hints for the bridge.
    std::array<bool, kMaximumObjects + 1> touched_{};
    std::uint64_t list_epoch_{};
    std::uint64_t next_generation_{};
    ObjectHandle first_active_{};
    ObjectHandle first_free_{};
    std::size_t active_count_{};
    std::size_t capacity_{kOriginalMaximumObjects};
    ObjectMemoryLayout layout_{ObjectMemoryLayout::original};
};

} // namespace starfox::simulation
