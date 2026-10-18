#include "updater_3ds.hpp"
#include "diagnostic_sha256.hpp"

#include <curl/curl.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace starfox::platform_3ds {
namespace {
constexpr const char* folder = "sdmc:/3ds/Starwing/update";
constexpr const char* partial = "sdmc:/3ds/Starwing/update/download.part";
constexpr std::size_t block_bytes = 64U * 1024U;
bool space_for(std::uint64_t bytes) {
    FS_ArchiveResource resource{};
    return R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&resource))
        && std::uint64_t(resource.freeClusters) * resource.clusterSize >= bytes;
}
}

struct Updater3ds::Transfer {
    Updater3ds* owner;
    std::string* metadata;
    FILE* file;
    std::uint32_t bytes{};
    DiagnosticSha256 sha;
};

Updater3ds::Updater3ds(const char* path) : homebrew_(envIsHomebrew()) {
    LightLock_Init(&lock_);
    status_.experimental = std::strstr(STARWING_BUILD_VERSION, "-E") != nullptr;
    if (FILE* file = std::fopen("sdmc:/3ds/Starwing/update/channel.cfg", "rb")) {
        const int value = std::fgetc(file);
        if (value == '0' || value == '1') status_.experimental = value == '1';
        std::fclose(file);
    }
    if (path && std::string_view(path).starts_with("sdmc:/")
        && std::string_view(path).ends_with(".3dsx")) launch_path_ = path;
}
Updater3ds::~Updater3ds() {
    cancel();
    if (thread_) { threadJoin(thread_, UINT64_MAX); threadFree(thread_); }
}
bool Updater3ds::cancelled() const noexcept { return cancel_.load() || aptShouldClose(); }
void Updater3ds::cancel() noexcept {
    if (status().state != UpdateState::installing) cancel_.store(true);
}
UpdateStatus Updater3ds::status() {
    LightLock_Lock(&lock_); const auto copy = status_; LightLock_Unlock(&lock_);
    return copy;
}
std::string Updater3ds::notes() {
    LightLock_Lock(&lock_);
    try {
        auto copy = notes_;
        LightLock_Unlock(&lock_); return copy;
    } catch (...) { LightLock_Unlock(&lock_); throw; }
}
void Updater3ds::publish(UpdateState state, const char* message, unsigned percent) {
    LightLock_Lock(&lock_);
    status_.state = state; status_.percent = std::min(percent, 100U);
    std::snprintf(status_.message, sizeof(status_.message), "%s", message);
    if (release_) std::snprintf(status_.version, sizeof(status_.version), "%s", release_->version.c_str());
    LightLock_Unlock(&lock_);
}
void Updater3ds::check() { start(false); }
void Updater3ds::set_channel(bool experimental) {
    if (busy()) return;
    LightLock_Lock(&lock_); status_.experimental = experimental; LightLock_Unlock(&lock_);
    mkdir(folder, 0777);
    if (FILE* file = std::fopen("sdmc:/3ds/Starwing/update/channel.cfg", "wb")) {
        std::fputc(experimental ? '1' : '0', file); std::fclose(file);
    }
    check();
}
void Updater3ds::install() { if (status().state == UpdateState::available) start(true); }
void Updater3ds::start(bool install) {
    if (busy_.exchange(true)) return;
    if (thread_) { threadJoin(thread_, UINT64_MAX); threadFree(thread_); thread_ = nullptr; }
    if (!install) {
        release_.reset();
        LightLock_Lock(&lock_);
        status_.version[0] = 0; notes_.clear(); ++status_.notes_revision;
        LightLock_Unlock(&lock_);
    }
    cancel_.store(false); installing_ = install;
    publish(install ? UpdateState::downloading : UpdateState::checking,
        install ? "DOWNLOADING" : "CHECKING RELEASES");
    thread_ = threadCreate(worker, this, 128U * 1024U, 0x28, -2, false);
    if (!thread_) { busy_.store(false); publish(UpdateState::error, "CANNOT START NETWORK WORKER"); }
}
void Updater3ds::worker(void* context) { static_cast<Updater3ds*>(context)->run(); }

std::size_t Updater3ds::receive(char* data, std::size_t a, std::size_t b, void* context) {
    auto& t = *static_cast<Transfer*>(context);
    if (a && b > SIZE_MAX / a) return 0;
    const auto size = a * b;
    const auto limit = t.file ? t.owner->release_->bytes : 2U * 1024U * 1024U;
    if (t.owner->cancelled() || t.bytes > limit || size > limit - t.bytes) return 0;
    try {
        if (t.file) {
            if (std::fwrite(data, 1, size, t.file) != size) return 0;
            t.sha.update({reinterpret_cast<const std::uint8_t*>(data), size});
            const auto percent = static_cast<unsigned>((std::uint64_t(t.bytes) + size) * 100U / limit);
            t.owner->publish(UpdateState::downloading, "DOWNLOADING", percent);
        } else t.metadata->append(data, size);
    } catch (...) { return 0; }
    t.bytes += static_cast<std::uint32_t>(size);
    return size;
}
int Updater3ds::progress(void* owner, std::int64_t, std::int64_t, std::int64_t, std::int64_t) {
    return static_cast<Updater3ds*>(owner)->cancelled() ? 1 : 0;
}
bool Updater3ds::transfer(const char* url, std::string* metadata, FILE* file) {
    auto* curl = curl_easy_init();
    if (!curl) return false;
    Transfer transfer{this, metadata, file, 0, {}};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Starwing-3DS/" STARWING_BUILD_VERSION);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/update-ca.crt");
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, file ? 600L : 90L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 128L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    const auto rc = curl_easy_perform(curl);
    long code{}; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || code != 200) return false;
    if (!file) return true;
    const auto digest = transfer.sha.finish();
    char hex[65]{};
    for (unsigned i = 0; i < digest.size(); ++i) std::snprintf(hex + i * 2U, 3, "%02x", digest[i]);
    return transfer.bytes == release_->bytes && hex == release_->sha256;
}

bool Updater3ds::install_cia() {
    auto data = std::make_unique<std::array<std::uint8_t, block_bytes>>();
    if (R_FAILED(amInit())) return false;
    Handle input{}, output{}; bool started = false, ok = false;
    const auto open = FSUSER_OpenFileDirectly(&input, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Starwing/update/download.part"), FS_OPEN_READ, 0);
    if (R_SUCCEEDED(open)) {
        AM_TitleInfo info{}; std::uint64_t required{};
        if (R_SUCCEEDED(AM_GetCiaFileInfo(MEDIATYPE_SD, &info, input))
            && info.titleID == update_title_id && info.version > STARWING_CIA_VERSION
            && R_SUCCEEDED(AM_GetCiaRequiredSpace(&required, MEDIATYPE_SD, input))
            && space_for(required + 4U * 1024U * 1024U)
            && R_SUCCEEDED(AM_StartCiaInstallOverwrite(&output, MEDIATYPE_SD))) {
            started = true; std::uint64_t offset{};
            while (offset < release_->bytes) {
                const auto want = static_cast<std::uint32_t>(std::min<std::uint64_t>(block_bytes, release_->bytes - offset));
                std::uint32_t read{}, written{};
                if (R_FAILED(FSFILE_Read(input, &read, offset, data->data(), want)) || read != want
                    || R_FAILED(FSFILE_Write(output, &written, offset, data->data(), want, 0))
                    || written != want) break;
                offset += want;
                publish(UpdateState::installing, "INSTALLING - DO NOT POWER OFF",
                    static_cast<unsigned>(offset * 100U / release_->bytes));
            }
            if (offset == release_->bytes) {
                ok = R_SUCCEEDED(AM_FinishCiaInstall(output));
                started = false;
            }
        }
        if (started) AM_CancelCIAInstall(output);
        FSFILE_Close(input);
    }
    amExit(); return ok;
}
bool Updater3ds::install_3dsx() {
    if (launch_path_.empty()) return false;
    const auto backup = launch_path_ + ".bak-" STARWING_BUILD_VERSION;
    if (access(backup.c_str(), F_OK) == 0) return false;
    if (std::rename(launch_path_.c_str(), backup.c_str()) != 0) return false;
    if (std::rename(partial, launch_path_.c_str()) != 0) {
        std::rename(backup.c_str(), launch_path_.c_str()); return false;
    }
    return true;
}

void Updater3ds::run() {
    void* sockets = nullptr;
    bool ac = false, soc = false, ssl = false, curl = false;
    FILE* file = nullptr;
    try {
        ac = R_SUCCEEDED(acInit()); u32 wifi{};
        if (!ac || R_FAILED(ACU_GetWifiStatus(&wifi)) || !wifi)
            throw std::runtime_error("CONNECT WI-FI AND RETRY");
        sockets = memalign(4096U, 1024U * 1024U);
        if (!sockets || R_FAILED(socInit(static_cast<std::uint32_t*>(sockets), 1024U * 1024U)))
            throw std::runtime_error("NETWORK INIT FAILED");
        soc = true;
        ssl = R_SUCCEEDED(sslcInit(0));
        if (!ssl) throw std::runtime_error("TLS INIT FAILED");
        curl = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
        if (!curl) throw std::runtime_error("NETWORK INIT FAILED");
        if (!installing_) {
            const bool experimental = status().experimental;
            std::string json;
            if (!transfer(experimental ? update_experimental_api : update_api, &json, nullptr))
                throw std::runtime_error("RELEASE CHECK FAILED - RETRY");
            release_ = parse_update_release(json, homebrew_, experimental);
            if (!release_) publish(UpdateState::empty, experimental
                ? "NO EXPERIMENTAL RELEASE" : "NO COMPATIBLE STABLE RELEASE");
            else {
                LightLock_Lock(&lock_); notes_.swap(release_->notes);
                ++status_.notes_revision; LightLock_Unlock(&lock_);
                const bool newer = update_is_newer(release_->version, STARWING_BUILD_VERSION);
                publish(newer ? UpdateState::available : UpdateState::current,
                    newer ? "UPDATE AVAILABLE" : "YOU ARE UP TO DATE");
            }
        } else {
            if (homebrew_ && launch_path_.empty()) throw std::runtime_error("3DSX LAUNCH PATH UNKNOWN");
            if (!space_for(std::uint64_t(release_->bytes) * 2U + 16U * 1024U * 1024U))
                throw std::runtime_error("NOT ENOUGH SD SPACE");
            mkdir(folder, 0777);
            file = std::fopen(partial, "wb");
            if (!file || !transfer(release_->url.c_str(), nullptr, file))
                throw std::runtime_error("DOWNLOAD OR HASH CHECK FAILED");
            const bool flushed = std::fflush(file) == 0;
            const bool closed = std::fclose(file) == 0; file = nullptr;
            if (!flushed || !closed) throw std::runtime_error("SD WRITE FAILED");
            if (cancelled() || !aptIsActive()) throw std::runtime_error("UPDATE CANCELLED");
            const bool home = aptIsHomeAllowed(), sleep = aptIsSleepAllowed();
            aptSetHomeAllowed(false); aptSetSleepAllowed(false);
            publish(UpdateState::installing, "INSTALLING - DO NOT POWER OFF");
            bool installed = false;
            try { installed = homebrew_ ? install_3dsx() : install_cia(); }
            catch (...) { installed = false; }
            aptSetSleepAllowed(sleep); aptSetHomeAllowed(home);
            if (!installed) throw std::runtime_error("INSTALL FAILED - KEEP OLD COPY");
            publish(UpdateState::done, "UPDATED - REOPEN STARWING 3DS",100U);
        }
    } catch (const std::exception& e) { publish(UpdateState::error, cancelled() ? "UPDATE CANCELLED" : e.what()); }
    catch (...) { publish(UpdateState::error, "UPDATE FAILED"); }
    if (file) std::fclose(file);
    if (installing_) std::remove(partial);
    if (curl) curl_global_cleanup();
    if (ssl) sslcExit();
    if (soc) socExit();
    std::free(sockets);
    if (ac) acExit();
    busy_.store(false);
}
} // namespace starfox::platform_3ds
