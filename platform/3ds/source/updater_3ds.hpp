#pragma once

#include "update_manifest.hpp"
#include <3ds.h>
#include <atomic>
#include <cstdio>

namespace starfox::platform_3ds {
enum class UpdateState { idle, checking, empty, current, available, downloading, installing, done, error };
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
    explicit Updater3ds(const char* launch_path);
    ~Updater3ds();
    Updater3ds(const Updater3ds&) = delete;
    Updater3ds& operator=(const Updater3ds&) = delete;
    void check();
    void set_channel(bool experimental);
    void install();
    void cancel() noexcept;
    UpdateStatus status();
    std::string notes();
    bool busy() const noexcept { return busy_.load(); }
private:
    friend struct Updater3dsTest;
    static void worker(void* self);
    void run();
    void start(bool install);
    void publish(UpdateState state, const char* message, unsigned percent = 0);
    bool transfer(const char* url, std::string* metadata, FILE* file);
    bool install_cia();
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
