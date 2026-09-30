#include "diagnostics_3ds.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

namespace starfox::platform_3ds {
namespace {
constexpr const char* root = "sdmc:/3ds/Starwing/dumps";
constexpr std::size_t chunk_size = 1024U * 1024U;

bool managed_dump_name(const char* name) {
    if (std::strncmp(name, "dump-", 5) == 0) return true;
    const char* p = name;
    while (*p >= '0' && *p <= '9') ++p;
    return p != name && std::strncmp(p, "-dump-", 6) == 0;
}

// Match the other 3DS ports: derive the next number from numbered folders.
// Legacy unnumbered captures remain intact and do not consume sequence IDs.
bool next_dump_number(unsigned& next) {
    DIR* entries = opendir(root);
    if (!entries) return false;
    next = 0;
    bool valid = true;
    while (dirent* entry = readdir(entries)) {
        const char* name = entry->d_name;
        const char* p = name;
        unsigned number = 0;
        while (*p >= '0' && *p <= '9') {
            const unsigned digit = static_cast<unsigned>(*p++ - '0');
            if (number > (999999U - digit) / 10U) { valid = false; break; }
            number = number * 10U + digit;
        }
        if (!valid) break;
        if (p != name && std::strncmp(p, "-dump-", 6) == 0)
            next = std::max(next, number + 1U);
    }
    if (closedir(entries) != 0) valid = false;
    return valid && next <= 999999U;
}

// A fixed root, managed names, lstat and a depth bound keep the delete chord
// inside Starwing dumps even when an unexpected entry is present on the SD.
bool remove_dump_tree(const std::string& path, unsigned depth) {
    if (depth > 8U) return false;
    DIR* entries = opendir(path.c_str());
    if (!entries) return false;
    bool ok = true;
    while (dirent* entry = readdir(entries)) {
        if (std::strcmp(entry->d_name, ".") == 0
            || std::strcmp(entry->d_name, "..") == 0) continue;
        const auto child = path + "/" + entry->d_name;
        struct stat info{};
        if (lstat(child.c_str(), &info) != 0) { ok = false; continue; }
        if (S_ISDIR(info.st_mode)) {
            if (!remove_dump_tree(child, depth + 1U)) ok = false;
        } else if (unlink(child.c_str()) != 0) {
            ok = false;
        }
    }
    if (closedir(entries) != 0) ok = false;
    if (ok && rmdir(path.c_str()) != 0) ok = false;
    return ok;
}

bool make_dir(const std::string& path) {
    return mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
}
bool write_bytes(const std::string& path, const void* bytes, std::size_t size) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(bytes, 1, size, f) == size;
    const bool closed = std::fclose(f) == 0;
    if (!ok || !closed) std::remove(path.c_str());
    return ok && closed;
}
bool write_text(const std::string& path, const char* text) {
    return text && write_bytes(path, text, std::strlen(text));
}
void be32(std::vector<u8>& out, u32 n) {
    out.push_back(static_cast<u8>(n >> 24)); out.push_back(static_cast<u8>(n >> 16));
    out.push_back(static_cast<u8>(n >> 8)); out.push_back(static_cast<u8>(n));
}
u32 crc32(const u8* data, std::size_t size) {
    u32 c = 0xffffffffU;
    for (std::size_t i = 0; i < size; ++i) {
        c ^= data[i];
        for (unsigned j = 0; j < 8; ++j)
            c = (c >> 1) ^ ((c & 1U) ? 0xedb88320U : 0U);
    }
    return ~c;
}
void png_chunk(std::vector<u8>& png, const char type[4], const std::vector<u8>& body) {
    be32(png, static_cast<u32>(body.size()));
    const auto start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), body.begin(), body.end());
    be32(png, crc32(png.data() + start, png.size() - start));
}
unsigned pixel_bytes(GSPGPU_FramebufferFormat format) {
    switch (format) {
    case GSP_RGBA8_OES: return 4;
    case GSP_BGR8_OES: return 3;
    case GSP_RGB565_OES: case GSP_RGB5_A1_OES: case GSP_RGBA4_OES: return 2;
    default: return 0;
    }
}
void decode_pixel(const u8* p, GSPGPU_FramebufferFormat format, u8* rgb) {
    switch (format) {
    case GSP_RGBA8_OES: rgb[0] = p[3]; rgb[1] = p[2]; rgb[2] = p[1]; break;
    case GSP_BGR8_OES: rgb[0] = p[2]; rgb[1] = p[1]; rgb[2] = p[0]; break;
    case GSP_RGB565_OES: {
        const u16 v = static_cast<u16>(p[0] | (p[1] << 8));
        rgb[0] = static_cast<u8>(((v >> 11) & 31U) * 255U / 31U);
        rgb[1] = static_cast<u8>(((v >> 5) & 63U) * 255U / 63U);
        rgb[2] = static_cast<u8>((v & 31U) * 255U / 31U); break;
    }
    case GSP_RGB5_A1_OES: {
        const u16 v = static_cast<u16>(p[0] | (p[1] << 8));
        rgb[0] = static_cast<u8>(((v >> 11) & 31U) * 255U / 31U);
        rgb[1] = static_cast<u8>(((v >> 6) & 31U) * 255U / 31U);
        rgb[2] = static_cast<u8>(((v >> 1) & 31U) * 255U / 31U); break;
    }
    case GSP_RGBA4_OES: {
        const u16 v = static_cast<u16>(p[0] | (p[1] << 8));
        rgb[0] = static_cast<u8>(((v >> 12) & 15U) * 17U);
        rgb[1] = static_cast<u8>(((v >> 8) & 15U) * 17U);
        rgb[2] = static_cast<u8>(((v >> 4) & 15U) * 17U); break;
    }
    default: rgb[0] = rgb[1] = rgb[2] = 0;
    }
}
bool screenshot(const std::string& path, gfxScreen_t screen, unsigned expected_width,
                const u8* submitted_framebuffer, bool cpu_snapshot) {
    u16 physical_width = 0, physical_height = 0;
    const auto* drawing_framebuffer = gfxGetFramebuffer(screen, GFX_LEFT,
        &physical_width, &physical_height);
    const auto* fb = submitted_framebuffer ? submitted_framebuffer : drawing_framebuffer;
    const auto format = gfxGetScreenFormat(screen);
    const unsigned bpp = pixel_bytes(format);
    // libctru returns rotated dimensions: 240 columns by 400/320 rows.
    if (!fb || physical_width != 240 || physical_height != expected_width || !bpp)
        return false;
    const auto fb_size = static_cast<u32>(physical_width) * physical_height * bpp;
    if (!cpu_snapshot && R_FAILED(GSPGPU_InvalidateDataCache(fb, fb_size)))
        return false;
    std::vector<u8> raw;
    raw.reserve((expected_width * 3U + 1U) * 240U);
    for (unsigned y = 0; y < 240; ++y) {
        raw.push_back(0); // PNG filter None.
        for (unsigned x = 0; x < expected_width; ++x) {
            const std::size_t offset = (x * 240U + (239U - y)) * bpp;
            u8 rgb[3]; decode_pixel(fb + offset, format, rgb);
            raw.insert(raw.end(), rgb, rgb + 3);
        }
    }
    // A zlib stream with uncompressed DEFLATE blocks avoids a new library.
    std::vector<u8> z{0x78, 0x01};
    u32 a = 1, b = 0;
    for (std::size_t off = 0; off < raw.size();) {
        const u16 n = static_cast<u16>(std::min<std::size_t>(65535, raw.size() - off));
        z.push_back(off + n == raw.size() ? 1 : 0);
        z.push_back(static_cast<u8>(n)); z.push_back(static_cast<u8>(n >> 8));
        z.push_back(static_cast<u8>(~n)); z.push_back(static_cast<u8>((~n) >> 8));
        for (unsigned i = 0; i < n; ++i) {
            const u8 c = raw[off + i]; z.push_back(c);
            a = (a + c) % 65521U; b = (b + a) % 65521U;
        }
        off += n;
    }
    be32(z, (b << 16) | a);
    std::vector<u8> png{137,80,78,71,13,10,26,10};
    std::vector<u8> ihdr; be32(ihdr, expected_width); be32(ihdr, 240);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    png_chunk(png, "IHDR", ihdr); png_chunk(png, "IDAT", z);
    png_chunk(png, "IEND", {});
    return write_bytes(path, png.data(), png.size());
}
bool log_tail(const std::string& output, const char* input) {
    if (!input) return write_text(output, "No log path supplied.\n");
    FILE* f = std::fopen(input, "rb");
    if (!f) return write_text(output, "Log unavailable.\n");
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return false; }
    const long end = std::ftell(f);
    if (end < 0 || std::fseek(f, std::max(0L, end - 16384L), SEEK_SET) != 0) {
        std::fclose(f); return false;
    }
    std::vector<char> bytes(static_cast<std::size_t>(std::min(16384L, end)));
    const bool ok = std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok && write_bytes(output, bytes.data(), bytes.size());
}
bool hash_file(const std::string& dir, const char* name, std::string& hashes) {
    FILE* file = std::fopen((dir + "/" + name).c_str(), "rb");
    if (!file) return false;
    if (std::fseek(file, 0, SEEK_END) != 0) { std::fclose(file); return false; }
    const long length = std::ftell(file);
    if (length < 0 || length > static_cast<long>(chunk_size)
        || std::fseek(file, 0, SEEK_SET) != 0) { std::fclose(file); return false; }
    std::vector<u8> data(static_cast<std::size_t>(length));
    const bool read = std::fread(data.data(), 1, data.size(), file) == data.size();
    std::fclose(file);
    u8 hash[32]{};
    if (!read || R_FAILED(FSUSER_UpdateSha256Context(data.data(), data.size(), hash))) return false;
    char hex[65];
    for (unsigned i = 0; i < 32; ++i)
        std::snprintf(hex + i * 2, sizeof(hex) - i * 2, "%02x", hash[i]);
    hashes += std::string(hex) + "  " + name + "\n";
    return true;
}
std::string json_string(const char* s) {
    std::string out{"\""};
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s ? s : ""); *p; ++p) {
        if (*p == '"' || *p == '\\') { out += '\\'; out += static_cast<char>(*p); }
        else if (*p < 32) { char b[7]; std::snprintf(b, sizeof(b), "\\u%04x", *p); out += b; }
        else out += static_cast<char>(*p);
    }
    return out + '"';
}
bool free_sd_bytes(u64& bytes) {
    struct statvfs info{};
    if (statvfs(root, &info) == 0) {
        const u64 block_size = info.f_frsize != 0 ? info.f_frsize : info.f_bsize;
        if (block_size != 0 && info.f_bavail <= UINT64_MAX / block_size) {
            bytes = static_cast<u64>(info.f_bavail) * block_size;
            return true;
        }
    }
    // The SD devoptab uses this archive-resource call for statvfs. Retry it
    // directly when a custom mount rejects the path passed above.
    FS_ArchiveResource resource{};
    if (R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&resource))
        && resource.clusterSize != 0
        && resource.freeClusters <= UINT64_MAX / resource.clusterSize) {
        bytes = static_cast<u64>(resource.freeClusters) * resource.clusterSize;
        return true;
    }
    return false;
}
bool full_memory(const std::string& dir, std::string& hashes, std::string& error,
    u64 total, const DiagnosticReport3ds& report) {
    FILE* map = std::fopen((dir + "/memory-map.txt").c_str(), "wb");
    if (!map) { error = "memory map open failed"; return false; }
    std::fprintf(map, "Readable current-process mappings; binary files are private.\n");
    unsigned part = 0;
    u64 written = 0;
    for (u32 address = 0x00100000U; address < 0x40000000U;) {
        MemInfo info{}; PageInfo page{};
        if (R_FAILED(svcQueryMemory(&info, &page, address)) || info.size == 0) {
            std::fclose(map); error = "memory query failed"; return false;
        }
        const u64 next = static_cast<u64>(info.base_addr) + info.size;
        const bool readable = (info.perm & MEMPERM_READ) && info.state != MEMSTATE_IO
            && info.state != MEMSTATE_FREE && next <= 0x40000000ULL;
        std::fprintf(map, "%08lx %08lx state=%lu perm=%lu %s\n",
            static_cast<unsigned long>(info.base_addr), static_cast<unsigned long>(info.size),
            static_cast<unsigned long>(info.state), static_cast<unsigned long>(info.perm),
            readable ? "captured" : "skipped");
        if (readable) {
            for (u64 pos = info.base_addr; pos < next;) {
                const auto size = static_cast<u32>(std::min<u64>(chunk_size, next - pos));
                char name[32]; std::snprintf(name, sizeof(name), "mem-%03u.bin", part++);
                std::fprintf(map, "  %s address=%08lx bytes=%lu\n", name,
                    static_cast<unsigned long>(pos), static_cast<unsigned long>(size));
                if (!write_bytes(dir + "/" + name, reinterpret_cast<const void*>(static_cast<uintptr_t>(pos)), size)) {
                    std::fclose(map); error = "memory chunk write failed"; return false;
                }
                // Hash the committed bytes, not a changing live region.
                if (!hash_file(dir, name, hashes)) {
                    std::fclose(map); error = "SHA256 service failed"; return false;
                }
                pos += size;
                written += size;
                if (report.progress_callback)
                    report.progress_callback(report.progress_context,
                        "WRITING MEMORY", written, total);
            }
        }
        if (next <= address) {
            std::fclose(map);
            error = "memory map did not advance"; return false;
        }
        if (next > 0xffffffffULL) break;
        address = static_cast<u32>(next);
    }
    if (std::fclose(map) != 0) { error = "memory map close failed"; return false; }
    return true;
}
bool readable_memory_bytes(u64& total) {
    total = 0;
    for (u32 address = 0x00100000U; address < 0x40000000U;) {
        MemInfo info{}; PageInfo page{};
        if (R_FAILED(svcQueryMemory(&info, &page, address)) || info.size == 0)
            return false;
        const u64 next = static_cast<u64>(info.base_addr) + info.size;
        if ((info.perm & MEMPERM_READ) && info.state != MEMSTATE_IO
            && info.state != MEMSTATE_FREE && next <= 0x40000000ULL) total += info.size;
        if (next <= address) return false;
        if (next > 0xffffffffULL) break;
        address = static_cast<u32>(next);
    }
    return true;
}
} // namespace

DiagnosticKind3ds Diagnostics3ds::poll_shortcut(u32 down, u32 held, bool& requested) noexcept {
    const bool chord = (held & (KEY_L | KEY_R)) == (KEY_L | KEY_R)
        && (held & (KEY_A | KEY_B)) != 0;
    requested = chord && !shortcut_latched_
        && ((down & (KEY_L | KEY_R | KEY_A | KEY_B)) != 0);
    shortcut_latched_ = chord;
    return (held & KEY_B) ? DiagnosticKind3ds::full : DiagnosticKind3ds::quick;
}
bool Diagnostics3ds::poll_clear_shortcut(u32 down, u32 held) noexcept {
    const bool chord = (held & (KEY_L | KEY_R | KEY_X))
        == (KEY_L | KEY_R | KEY_X);
    const bool requested = chord && !clear_latched_
        && (down & (KEY_L | KEY_R | KEY_X)) != 0U;
    clear_latched_ = chord;
    return requested;
}
Diagnostics3ds::Result Diagnostics3ds::clear_dumps() {
    if (busy_) return {false, {}, "Diagnostic operation already running"};
    busy_ = true;
    struct Unlock { bool& flag; ~Unlock() { flag = false; } } unlock{busy_};
    DIR* entries = opendir(root);
    if (!entries) {
        if (errno == ENOENT) return {true, root, "No dumps to delete"};
        return {false, root, "Cannot open dumps directory"};
    }
    unsigned removed = 0;
    bool ok = true;
    while (dirent* entry = readdir(entries)) {
        if (!managed_dump_name(entry->d_name)) continue;
        const auto path = std::string(root) + "/" + entry->d_name;
        struct stat info{};
        if (lstat(path.c_str(), &info) != 0) { ok = false; continue; }
        const bool deleted = S_ISDIR(info.st_mode)
            ? remove_dump_tree(path, 0U) : unlink(path.c_str()) == 0;
        if (deleted) ++removed;
        else ok = false;
    }
    if (closedir(entries) != 0) ok = false;
    return {ok, root, ok ? std::to_string(removed) + " dumps deleted"
                         : "Some dumps could not be deleted"};
}
bool Diagnostics3ds::poll_touch(u32 down, const touchPosition& touch,
                               int x, int y, int width, int height) noexcept {
    return (down & KEY_TOUCH) && touch.px >= x && touch.py >= y
        && touch.px < x + width && touch.py < y + height;
}
Diagnostics3ds::Result Diagnostics3ds::capture(DiagnosticKind3ds kind,
                                                const DiagnosticReport3ds& report) {
    if (busy_) return {false, {}, "Diagnostic capture already running"};
    busy_ = true;
    struct Unlock { bool& flag; ~Unlock() { flag = false; } } unlock{busy_};
    const auto progress = [&](const char* stage, u64 written, u64 total) {
        if (kind == DiagnosticKind3ds::full && report.progress_callback)
            report.progress_callback(report.progress_context, stage, written, total);
    };
    progress("CHECKING MEMORY", 0, 0);
    if (!make_dir("sdmc:/3ds") || !make_dir("sdmc:/3ds/Starwing") || !make_dir(root))
        return {false, {}, "Cannot create diagnostic directory"};
    u64 memory = 0;
    if (kind == DiagnosticKind3ds::full && !readable_memory_bytes(memory))
        return {false, {}, "Memory map query failed"};
    u64 free = 0;
    const bool space_known = free_sd_bytes(free);
    const u64 required = memory + 2U * 1024U * 1024U;
    if (space_known && free < required) {
        char message[80]{};
        std::snprintf(message, sizeof(message), "SD FREE %lluMB NEED %lluMB",
            static_cast<unsigned long long>(free / (1024U * 1024U)),
            static_cast<unsigned long long>((required + 1024U * 1024U - 1U) / (1024U * 1024U)));
        return {false, {}, message};
    }
    progress("SAVING FILES", 0, memory);
    // An unavailable query must not block a real SD write. The file writes
    // below still fail safely and leave an incomplete .partial directory.
    unsigned next = 0;
    if (!next_dump_number(next)) return {false, {}, "Cannot number dump"};
    std::string base;
    bool unique = false;
    for (unsigned n = 0; n < 100; ++n) {
        char prefix[32]{};
        std::snprintf(prefix, sizeof(prefix), "%03u-dump-", next);
        base = std::string(root) + "/" + prefix + std::to_string(osGetTime())
            + "-" + std::to_string(n)
            + (kind == DiagnosticKind3ds::full ? "-full" : "-quick");
        struct stat st{};
        if (stat(base.c_str(), &st) != 0 && stat((base + ".partial").c_str(), &st) != 0) {
            unique = true; break;
        }
    }
    if (!unique || !make_dir(base + ".partial")) return {false, {}, "Cannot create capture"};
    const std::string dir = base + ".partial";
    const std::string manifest = std::string("{\"schema\":1,\"kind\":")
        + json_string(kind == DiagnosticKind3ds::full ? "full" : "quick")
        + ",\"build\":" + json_string(report.build) + ",\"complete\":false}\n";
    if (!write_text(dir + "/manifest.json", manifest.c_str()))
        return {false, dir, "Manifest write failed"};
    const bool common = write_text(dir + "/runtime.json", report.runtime_json)
        && write_text(dir + "/performance.csv", report.performance_csv)
        && screenshot(dir + "/top.png", GFX_TOP, 400, report.top_framebuffer,
            report.framebuffer_snapshots)
        && screenshot(dir + "/bottom.png", GFX_BOTTOM, 320, report.bottom_framebuffer,
            report.framebuffer_snapshots)
        && write_text(dir + "/renderer.txt", report.renderer_text)
        && write_text(dir + "/audio.txt", report.audio_text)
        && write_text(dir + "/config.txt", report.config_text)
        && log_tail(dir + "/log-tail.txt", report.log_path);
    if (!common) return {false, dir, "Capture file or physical framebuffer failed"};
    std::string hashes, error;
    if (kind == DiagnosticKind3ds::full
        && !full_memory(dir, hashes, error, memory, report))
        return {false, dir, error};
    if (kind == DiagnosticKind3ds::full) {
        progress("CHECKING FILES", memory, memory);
        constexpr const char* common_names[] = {"runtime.json", "performance.csv",
            "top.png", "bottom.png", "renderer.txt", "audio.txt", "config.txt",
            "log-tail.txt", "memory-map.txt"};
        for (const char* name : common_names)
            if (!hash_file(dir, name, hashes)) return {false, dir, "Checksum failed"};
        if (!write_text(dir + "/SHA256SUMS.txt", hashes.c_str()))
            return {false, dir, "Checksum list write failed"};
    }
    const std::string done = std::string("{\"schema\":1,\"kind\":")
        + json_string(kind == DiagnosticKind3ds::full ? "full" : "quick")
        + ",\"build\":" + json_string(report.build) + ",\"complete\":true}\n";
    if (!write_text(dir + "/manifest.json.tmp", done.c_str())
        || std::remove((dir + "/manifest.json").c_str()) != 0
        || std::rename((dir + "/manifest.json.tmp").c_str(),
            (dir + "/manifest.json").c_str()) != 0)
        return {false, dir, "Final manifest write failed"};
    if (!write_text(dir + "/COMPLETE", "Capture complete. Keep private.\n"))
        return {false, dir, "Complete marker write failed"};
    if (std::rename(dir.c_str(), base.c_str()) != 0)
        return {false, dir, "Final directory commit failed"};
    progress("DUMP COMPLETE", memory, memory);
    return {true, base, "Diagnostic captured on SD"};
}
} // namespace starfox::platform_3ds
