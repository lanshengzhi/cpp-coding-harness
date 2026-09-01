#pragma once

#include "RuntimeRoot.hpp"
#include "WorkspaceFileSystem.hpp"

#include <cch/support/AsyncResult.hpp>

#include <cstddef>
#include <expected>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

namespace cch::harness::filesystem_detail {

/// Conservative per-admitted-operation byte charge covering retained result
/// state (read/list buffers, process results) that is not derivable from the
/// request bytes alone, so the runtime's byte bound stays conservative
/// (ADR 0040 admission/overload).
inline constexpr std::size_t kAdmittedOperationOverheadBytes{4096};

[[nodiscard]] inline std::size_t saturating_add(std::size_t left, std::size_t right) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return std::numeric_limits<std::size_t>::max();
    }
    return left + right;
}

[[nodiscard]] inline std::size_t file_read_charge(std::size_t path_size) noexcept {
    return saturating_add(path_size, kFileSystemCapacity.max_file_bytes);
}

[[nodiscard]] inline std::size_t text_lines_charge(std::size_t path_size) noexcept {
    return saturating_add(saturating_add(path_size, kFileSystemCapacity.max_text_lines_result_bytes),
            kFileSystemCapacity.max_text_lines * sizeof(std::string));
}

[[nodiscard]] inline std::size_t directory_list_charge(std::size_t path_size) noexcept {
    return saturating_add(saturating_add(path_size, kFileSystemCapacity.max_directory_result_bytes),
            kFileSystemCapacity.max_directory_entries * sizeof(FileInfo));
}

[[nodiscard]] inline FileError aborted_file_error(
        std::optional<std::string> path = std::nullopt, bool side_effects_may_remain = false) {
    return FileError{
            .code = FileErrorCode::Aborted,
            .message = side_effects_may_remain
                               ? "Operation aborted; already-performed filesystem side effects may remain"
                               : "Operation aborted",
            .path = std::move(path),
    };
}

[[nodiscard]] inline FileError busy_file_error(std::optional<std::string> path = std::nullopt) {
    return FileError{
            .code = FileErrorCode::Busy,
            .message = "Runtime is busy; filesystem operation was not admitted",
            .path = std::move(path),
    };
}

#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
[[nodiscard]] inline FileError unknown_file_error(std::optional<std::string> path = std::nullopt) {
    return FileError{
            .code = FileErrorCode::Unknown,
            .message = "filesystem operation could not be scheduled",
            .path = std::move(path),
    };
}
#endif

template <typename T> struct FileOperationState {
    std::optional<RuntimeTarget::Admission> admission;
    support::AsyncCompletion<T, FileError> completion;
    std::optional<std::expected<T, FileError>> outcome;

    /// Install the move-only admission and completion only after the shared
    /// allocation succeeds, so setup failures still advance the ordered
    /// mailbox at this admission's sequence. Must not be called twice.
    void install(RuntimeTarget::Admission admission, support::AsyncCompletion<T, FileError> completion) noexcept {
        this->admission = std::move(admission);
        this->completion = std::move(completion);
    }
};

template <typename T, typename Operation>
[[nodiscard]] support::AsyncResult<T, FileError> submit_filesystem_operation(
        std::shared_ptr<RuntimeTarget> runtime_target,
        std::shared_ptr<WorkspaceFileSystem> filesystem,
        std::size_t byte_charge,
        std::stop_token stop_token,
        std::optional<std::string> path,
        Operation operation,
        AdmissionLane lane = AdmissionLane::Ordinary) {
    if (stop_token.stop_requested()) {
        return support::AsyncResult<T, FileError>{std::unexpected(aborted_file_error(std::move(path)))};
    }

    return support::AsyncResult<T, FileError>{support::AsyncProducer<T, FileError>{
            [runtime_target = std::move(runtime_target),
                    filesystem = std::move(filesystem),
                    byte_charge,
                    stop_token,
                    path = std::move(path),
                    operation = std::move(operation),
                    lane](support::AsyncCompletion<T, FileError> completion) mutable noexcept {
                std::shared_ptr<FileOperationState<T>> state;
                // Keep admission out-of-line across state allocation so a
                // setup failure releases it through the ordered mailbox.
                std::optional<RuntimeTarget::Admission> admission;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                bool terminal_posted = false;
                try {
#endif
                    if (stop_token.stop_requested()) {
                        completion(std::unexpected(aborted_file_error(std::move(path))));
                        return;
                    }
                    if (!runtime_target) {
                        completion(std::unexpected(busy_file_error(std::move(path))));
                        return;
                    }
                    admission = lane == AdmissionLane::Reserved
                                        ? runtime_target->try_admit_reserved(
                                                  saturating_add(byte_charge, kAdmittedOperationOverheadBytes))
                                        : runtime_target->try_admit(
                                                  saturating_add(byte_charge, kAdmittedOperationOverheadBytes));
                    if (!admission) {
                        completion(std::unexpected(busy_file_error(std::move(path))));
                        return;
                    }
                    state = std::make_shared<FileOperationState<T>>();
                    state->install(std::move(*admission), std::move(completion));
                    const bool queued =
                            state->admission->post_worker([state,
                                                                  filesystem = std::move(filesystem),
                                                                  stop_token,
                                                                  path = std::move(path),
                                                                  operation = std::move(operation)]() mutable noexcept {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                                try {
#endif
                                    if (stop_token.stop_requested()) {
                                        state->outcome = std::unexpected(aborted_file_error(std::move(path)));
                                    } else {
                                        // Once the operation starts, return its actual outcome. In
                                        // particular, cancellation cannot rewrite a committed write,
                                        // remove, or temporary-resource creation as Aborted.
                                        state->outcome = operation(*filesystem);
                                    }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                                } catch (const std::exception& error) {
                                    state->outcome = std::unexpected(FileError{
                                            .code = FileErrorCode::Unknown,
                                            .message = error.what(),
                                            .path = std::move(path),
                                    });
                                } catch (...) {
                                    state->outcome = std::unexpected(FileError{
                                            .code = FileErrorCode::Unknown,
                                            .message = "filesystem operation failed",
                                            .path = std::move(path),
                                    });
                                }
#endif
                                std::move(*state->admission).complete([state]() mutable noexcept {
                                    state->completion(std::move(*state->outcome));
                                });
                            });
                    if (!queued) {
                        std::move(*state->admission).complete([state, path = std::move(path)]() mutable noexcept {
                            state->completion(std::unexpected(busy_file_error(std::move(path))));
                        });
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                        terminal_posted = true;
#endif
                    }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                } catch (const std::exception& error) {
                    const FileError failure{
                            .code = FileErrorCode::Unknown,
                            .message = error.what(),
                            .path = std::nullopt,
                    };
                    if (state && !terminal_posted) {
                        std::move(*state->admission).complete([state, failure = failure]() mutable noexcept {
                            state->completion(std::unexpected(std::move(failure)));
                        });
                    } else if (admission) {
                        std::move(*admission)
                                .complete([completion = std::move(completion), failure = failure]() mutable noexcept {
                                    completion(std::unexpected(std::move(failure)));
                                });
                    } else {
                        completion(std::unexpected(failure));
                    }
                } catch (...) {
                    const auto failure = unknown_file_error();
                    if (state && !terminal_posted) {
                        std::move(*state->admission).complete([state, failure = failure]() mutable noexcept {
                            state->completion(std::unexpected(std::move(failure)));
                        });
                    } else if (admission) {
                        std::move(*admission)
                                .complete([completion = std::move(completion), failure = failure]() mutable noexcept {
                                    completion(std::unexpected(failure));
                                });
                    } else {
                        completion(std::unexpected(failure));
                    }
                }
#endif
            }}};
}

} // namespace cch::harness::filesystem_detail
