#include "FileMutationQueue.hpp"

#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <utility>

namespace cch::durable {

struct FileMutationSlot {
    std::mutex mutex;
    std::condition_variable turn;
    std::size_t next_ticket{0};
    std::size_t now_serving{0};
};

namespace {

/// One slot per file for the whole process, so every environment that resolves
/// to the same file shares it and no call waits on another file.
// debt: one process-wide mutex serializes slot lookup and release for every file; upgrade to a sharded
// registry if unrelated files measurably contend on it.
std::mutex registry_mutex;
std::map<std::string, std::shared_ptr<FileMutationSlot>> registry;

} // namespace

FileMutationGuard::FileMutationGuard(std::string key) {
    {
        const std::lock_guard guard{registry_mutex};
        auto& slot = registry[key];
        if (!slot) {
            slot = std::make_shared<FileMutationSlot>();
        }
        slot_ = slot;
        ticket_ = slot_->next_ticket++;
    }
    key_ = std::move(key);
    std::unique_lock lock{slot_->mutex};
    slot_->turn.wait(lock, [this] { return slot_->now_serving == ticket_; });
}

FileMutationGuard::~FileMutationGuard() {
    const std::lock_guard guard{registry_mutex};
    {
        const std::lock_guard turn{slot_->mutex};
        ++slot_->now_serving;
    }
    slot_->turn.notify_all();
    // The last holder retires the slot so a long-lived process holds one entry
    // per file being written, not one per file ever written.
    if (slot_->next_ticket == slot_->now_serving) {
        registry.erase(key_);
    }
}

std::string mutation_key(Environment& environment, std::string_view path, std::stop_token stop) {
    std::filesystem::path absolute;
    if (auto info = environment.stat(path, stop); info) {
        absolute = info->path;
    } else if (const std::filesystem::path given{std::string(path)}; given.is_absolute()) {
        absolute = given;
    } else if (auto cwd = environment.stat(".", stop); cwd) {
        absolute = std::filesystem::path{cwd->path} / std::string(path);
    } else {
        return std::string(path);
    }
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(absolute, error);
    return error ? absolute.lexically_normal().string() : canonical.string();
}

FileMutationGuard acquire_file_mutation(std::string key) { return FileMutationGuard{std::move(key)}; }

} // namespace cch::durable
