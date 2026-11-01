#pragma once

#include <cch/durable/Environment.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace cch::durable {

/// One file's arrival-ordered mutation slot, shared by every environment that
/// resolves to it (private to the queue implementation).
struct FileMutationSlot;

/// Holds one file's mutation slot for the length of a read-modify-write.
/// `edit` and `write` calls on one file take the slot in arrival order, so a
/// concurrent pair applies one after another instead of losing an update; other
/// files never wait. This serializes this runtime's own calls, not `bash` or
/// another process.
class FileMutationGuard {
public:
    FileMutationGuard(FileMutationGuard&&) = delete;
    FileMutationGuard& operator=(FileMutationGuard&&) = delete;
    ~FileMutationGuard();
    FileMutationGuard(const FileMutationGuard&) = delete;
    FileMutationGuard& operator=(const FileMutationGuard&) = delete;

private:
    friend FileMutationGuard acquire_file_mutation(std::string key);

    explicit FileMutationGuard(std::string key);

    std::shared_ptr<FileMutationSlot> slot_;
    std::string key_;
    std::size_t ticket_{0};
};

/// The slot a call targets: the environment's absolute view of `path`,
/// canonical where the host resolves it, so two spellings of one file share a
/// slot. An environment that cannot resolve the path falls back to the given
/// spelling.
[[nodiscard]] std::string mutation_key(Environment& environment, std::string_view path, std::stop_token stop);

/// Wait for this file's turn; the guard releases the slot when it leaves scope.
[[nodiscard]] FileMutationGuard acquire_file_mutation(std::string key);

} // namespace cch::durable
