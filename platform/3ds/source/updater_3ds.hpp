#pragma once

#include "update_manifest.hpp"
#include <3ds.h>
#include <atomic>
#include <cstdio>

// The in-game updater: on a worker thread it checks GitHub releases for the
// chosen channel, downloads the CIA (or the 3DSX when running as homebrew),
// verifies its size and SHA-256 and installs it. Driven from game_3ds.cpp and
// the Options update page; tests/updater_install_test.cpp runs it on the host.

namespace starfox::platform_3ds {
// empty: no matching release; current: the installed build is the newest;
// available: install() can proceed; done: installed, the game must be reopened.
enum class UpdateState { idle, checking, empty, current, available, downloading, installing, done, error };
// A copy for the menu. percent tracks the download or install; notes_revision
// changes whenever notes() changes; experimental is the selected channel;
// message is upper-case text for the menu and version the release tag.
struct UpdateStatus {
    UpdateState state{UpdateState::idle};
    unsigned percent{};
    unsigned notes_revision{};
    bool experimental{};
    char message[48]{"READY TO CHECK"};
    char version[32]{};
};

class Updater3ds {
public:
    // launch_path is the running .3dsx on sdmc:/, which a homebrew update
    // replaces; anything else is ignored. The channel comes from
    // update/channel.cfg, else from this build (an "-E" version is experimental).
    explicit Updater3ds(const char* launch_path);
    // Cancels unless an install runs, then waits for the worker.
    ~Updater3ds();
    Updater3ds(const Updater3ds&) = delete;
    Updater3ds& operator=(const Updater3ds&) = delete;
    // Starts a release check; ignored while busy.
    void check();
    // Saves the channel and checks it; ignored while busy.
    void set_channel(bool experimental);
    // Downloads and installs the release found by the last check, only while
    // the state is available.
    void install();
    // Asks the worker to stop; an install in progress is never interrupted.
    void cancel() noexcept;
    // Thread-safe copies of the state and of the release notes.
    UpdateStatus status();
    std::string notes();
    bool busy() const noexcept { return busy_.load(); }
private:
    friend struct Updater3dsTest;
    static void worker(void* self);
    // The worker: brings up Wi-Fi, sockets, TLS and curl, then checks or
    // downloads and installs, and reports the outcome through publish().
    void run();
    void start(bool install);
    void publish(UpdateState state, const char* message, unsigned percent = 0);
    // HTTPS GET of `url` into `metadata` (at most 2 MiB) or into `file`, which
    // must receive exactly the release size and match its SHA-256. False on
    // any failure, a status other than 200 or cancellation.
    bool transfer(const char* url, std::string* metadata, FILE* file);
    // Installs the download through AM when it carries this title ID with a
    // higher CIA version than the running build and the SD card has room.
    bool install_cia();
    // Renames the running .3dsx to a .bak-<version> backup and moves the
    // download into its place, restoring it on failure. Refuses when that
    // backup already exists.
    bool install_3dsx();
    bool cancelled() const noexcept;
    static std::size_t receive(char*, std::size_t, std::size_t, void*);
    static int progress(void*, std::int64_t, std::int64_t, std::int64_t, std::int64_t);
    struct Transfer;
    LightLock lock_{};
    UpdateStatus status_{};
    std::string notes_;
    std::optional<UpdateRelease> release_;
    Thread thread_{};
    std::atomic<bool> busy_{false}, cancel_{false};
    bool installing_{}, homebrew_{};
    std::string launch_path_;
};
} // namespace starfox::platform_3ds
