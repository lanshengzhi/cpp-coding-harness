#include <cch/agent/harness/LocalExecutionEnv.hpp>

#include "ExecutionErrorClassification.hpp"
#include "RuntimeRoot.hpp"
#include "SyncLocalExecutionEnv.hpp"
#include "ai/AsyncResultBridge.hpp"
#include "harness/Process.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>

#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

namespace cch::harness {
namespace {

/// Conservative per-admitted-operation byte charge covering retained result
/// state (read/list buffers, process results) that is not derivable from the
/// request bytes alone, so the runtime's byte bound stays conservative
/// (ADR 0040 admission/overload).
inline constexpr std::size_t kAdmittedOperationOverheadBytes{4096};

[[nodiscard]] FileError aborted_file_error(
    std::optional<std::string> path = std::nullopt,
    bool side_effects_may_remain = false) {
    return FileError{
        .code = FileErrorCode::Aborted,
        .message = side_effects_may_remain
            ? "Operation aborted; already-performed filesystem side effects may remain"
            : "Operation aborted",
        .path = std::move(path),
    };
}

[[nodiscard]] FileError busy_file_error(std::optional<std::string> path = std::nullopt) {
    return FileError{
        .code = FileErrorCode::Busy,
        .message = "Runtime is busy; filesystem operation was not admitted",
        .path = std::move(path),
    };
}

[[nodiscard]] FileError unknown_file_error(std::optional<std::string> path = std::nullopt) {
    return FileError{
        .code = FileErrorCode::Unknown,
        .message = "filesystem operation could not be scheduled",
        .path = std::move(path),
    };
}

[[nodiscard]] ExecutionError busy_execution_error() {
    return ExecutionError{
        .code = ExecutionErrorCode::Busy,
        .message = "Runtime is busy; shell operation was not admitted",
    };
}

[[nodiscard]] ExecutionError unknown_execution_error() {
    return ExecutionError{
        .code = ExecutionErrorCode::Unknown,
        .message = "shell operation could not be scheduled",
    };
}

[[nodiscard]] ExecutionError classify_terminal_process_error(const support::Error& error) {
    auto classified = classify_process_execution_error(error);
    if (classified.code == ExecutionErrorCode::Aborted) {
        classified.message = "Operation aborted; already-performed process side effects may remain";
    }
    return classified;
}

template <typename T>
struct FileOperationState {
    std::optional<RuntimeTarget::Admission> admission;
    support::AsyncCompletion<T, FileError> completion;
    std::optional<std::expected<T, FileError>> outcome;

    /// Install the move-only admission and completion only after the shared
    /// allocation succeeds, so a setup failure can still deliver the typed
    /// failure through the ordered mailbox at this admission's sequence
    /// instead of losing the completion and wedging the FIFO head (§5.4 /
    /// ADR 0040). Must not be called twice.
    void install(
        RuntimeTarget::Admission admission,
        support::AsyncCompletion<T, FileError> completion) noexcept {
        this->admission = std::move(admission);
        this->completion = std::move(completion);
    }
};

template <typename T, typename Operation>
[[nodiscard]] support::AsyncResult<T, FileError> submit_filesystem_operation(
    std::shared_ptr<RuntimeTarget> runtime_target,
    std::shared_ptr<SyncLocalExecutionEnv> sync,
    std::size_t byte_charge,
    std::stop_token stop_token,
    std::optional<std::string> path,
    Operation operation) {
    if (stop_token.stop_requested()) {
        return support::AsyncResult<T, FileError>{
            std::unexpected(aborted_file_error(std::move(path)))};
    }

    return support::AsyncResult<T, FileError>{
        support::AsyncProducer<T, FileError>{
            [runtime_target = std::move(runtime_target),
             sync = std::move(sync),
             byte_charge,
             stop_token,
             path = std::move(path),
             operation = std::move(operation)](
                support::AsyncCompletion<T, FileError> completion) mutable noexcept {
                std::shared_ptr<FileOperationState<T>> state;
                // Kept out-of-line across the state allocation so a setup
                // failure can release the admission through the ordered
                // mailbox at its sequence (FIFO) instead of out of band.
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
                    admission =
                        runtime_target->try_admit(byte_charge + kAdmittedOperationOverheadBytes);
                    if (!admission) {
                        completion(std::unexpected(busy_file_error(std::move(path))));
                        return;
                    }
                    // Allocate the shared state before the move-only admission
                    // and completion are installed, so a setup failure can
                    // still deliver the typed failure through the ordered
                    // mailbox at this admission's sequence (FIFO, §5.4).
                    state = std::make_shared<FileOperationState<T>>();
                    state->install(std::move(*admission), std::move(completion));
                    const bool queued = state->admission->post_worker(
                        [state,
                         sync = std::move(sync),
                         stop_token,
                         path = std::move(path),
                         operation = std::move(operation)]() mutable noexcept {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                            try {
#endif
                                if (stop_token.stop_requested()) {
                                    state->outcome = std::unexpected(
                                        aborted_file_error(std::move(path)));
                                } else {
                                    state->outcome = operation(*sync);
                                    if (stop_token.stop_requested()) {
                                        state->outcome = std::unexpected(aborted_file_error(
                                            std::move(path), true));
                                    }
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
                            std::move(*state->admission).complete(
                                [state]() mutable noexcept {
                                    state->completion(std::move(*state->outcome));
                                });
                        });
                    if (!queued) {
                        // Admission succeeded but the worker queue is already
                        // stopping: deliver a typed Busy through the ordered
                        // mailbox so the admission is released in sequence.
                        std::move(*state->admission).complete(
                            [state, path = std::move(path)]() mutable noexcept {
                                state->completion(std::unexpected(
                                    busy_file_error(std::move(path))));
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
                        std::move(*state->admission).complete(
                            [state, failure = failure]() mutable noexcept {
                                state->completion(std::unexpected(std::move(failure)));
                            });
                    } else if (admission) {
                        // Allocation failed after admission: release the byte
                        // charge and deliver the typed failure through the
                        // ordered mailbox at this admission's sequence so the
                        // FIFO head advances (ADR 0040 mailbox ordering).
                        std::move(*admission).complete(
                            [completion = std::move(completion),
                             failure = failure]() mutable noexcept {
                                completion(std::unexpected(std::move(failure)));
                            });
                    } else {
                        completion(std::unexpected(failure));
                    }
                } catch (...) {
                    const auto failure = unknown_file_error();
                    if (state && !terminal_posted) {
                        std::move(*state->admission).complete(
                            [state, failure = failure]() mutable noexcept {
                                state->completion(std::unexpected(std::move(failure)));
                            });
                    } else if (admission) {
                        std::move(*admission).complete(
                            [completion = std::move(completion),
                             failure = failure]() mutable noexcept {
                                completion(std::unexpected(std::move(failure)));
                            });
                    } else {
                        completion(std::unexpected(failure));
                    }
                }
#endif
            }} };
}

struct ShellOperationState final : std::enable_shared_from_this<ShellOperationState> {
    std::optional<RuntimeTarget::Admission> admission;
    support::AsyncCompletion<ShellExecResult, ExecutionError> completion;
    std::shared_ptr<RuntimeTarget> runtime_target;
    std::shared_ptr<SyncLocalExecutionEnv> sync;
    std::optional<std::expected<harness::ProcessRequest, ExecutionError>> request;

    /// Install the move-only admission/completion and owned references only
    /// after the shared allocation succeeds, so a setup failure can still
    /// deliver the typed failure through the ordered mailbox at this
    /// admission's sequence instead of losing the completion and wedging the
    /// FIFO head (§5.4 / ADR 0040). Must not be called twice.
    void install(
        RuntimeTarget::Admission admission,
        support::AsyncCompletion<ShellExecResult, ExecutionError> completion,
        std::shared_ptr<RuntimeTarget> runtime_target,
        std::shared_ptr<SyncLocalExecutionEnv> sync) noexcept {
        this->admission = std::move(admission);
        this->completion = std::move(completion);
        this->runtime_target = std::move(runtime_target);
        this->sync = std::move(sync);
    }

    void complete(std::expected<ShellExecResult, ExecutionError> outcome) noexcept {
        std::move(*admission).complete(
            [self = shared_from_this(), outcome = std::move(outcome)]() mutable noexcept {
                self->completion(std::move(outcome));
            });
    }
};

struct ShellProcessOperation final {
    std::shared_ptr<AsyncProcessRunner> runner;
    harness::ProcessRequest request;

    [[nodiscard]] boost::asio::awaitable<support::Expected<harness::ProcessResult>> operator()() {
        return runner->run(std::move(request));
    }
};

[[nodiscard]] boost::asio::awaitable<void> run_shell_process(
    std::shared_ptr<ShellOperationState> state,
    std::shared_ptr<AsyncProcessRunner> runner) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        auto operation = std::make_shared<ShellProcessOperation>(ShellProcessOperation{
            .runner = std::move(runner),
            .request = std::move(**state->request),
        });
        auto process = co_await ai::detail::invoke_awaitable<
            ShellProcessOperation,
            support::Expected<harness::ProcessResult>>(std::move(operation));
        if (!process) {
            state->complete(std::unexpected(
                classify_terminal_process_error(process.error())));
        } else if (process->timed_out) {
            state->complete(std::unexpected(ExecutionError{
                .code = ExecutionErrorCode::Timeout,
                .message = "shell command timed out",
            }));
        } else {
            state->complete(state->sync->exec_result_from_process(*process));
        }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (...) {
        state->complete(std::unexpected(unknown_execution_error()));
    }
#endif
    co_return;
}

} // namespace

struct AsyncLocalExecutionEnv::Impl final {
    Impl(
        std::shared_ptr<RuntimeTarget> runtime_target,
        std::shared_ptr<SyncLocalExecutionEnv> sync)
        : runtime_target(std::move(runtime_target)), sync(std::move(sync)) {}

    std::shared_ptr<RuntimeTarget> runtime_target;
    std::shared_ptr<SyncLocalExecutionEnv> sync;
};

AsyncLocalExecutionEnv::AsyncLocalExecutionEnv(
    std::shared_ptr<RuntimeTarget> runtime_target,
    std::filesystem::path workspace,
    bool bash_enabled,
    std::vector<std::string> secret_environment_names,
    ShellConfig shell_config)
    : impl_(std::make_shared<Impl>(
          std::move(runtime_target),
          std::make_shared<SyncLocalExecutionEnv>(
              std::move(workspace),
              bash_enabled,
              std::move(secret_environment_names),
              std::move(shell_config)))) {}

AsyncLocalExecutionEnv::AsyncLocalExecutionEnv(AsyncLocalExecutionEnv&&) noexcept = default;
AsyncLocalExecutionEnv& AsyncLocalExecutionEnv::operator=(AsyncLocalExecutionEnv&&) noexcept = default;
AsyncLocalExecutionEnv::~AsyncLocalExecutionEnv() = default;

const std::filesystem::path& AsyncLocalExecutionEnv::workspace() const {
    return impl_->sync->workspace();
}

support::AsyncResult<std::string, FileError> AsyncLocalExecutionEnv::absolutePath(
    std::string path,
    std::stop_token stop_token) {
    return submit_filesystem_operation<std::string>(
        impl_->runtime_target,
        impl_->sync,
        path.size(),
        stop_token,
        path,
        [path = std::move(path)](const SyncLocalExecutionEnv& sync) {
            return sync.absolutePath(path);
        });
}

support::AsyncResult<std::string, FileError> AsyncLocalExecutionEnv::joinPath(
    std::vector<std::string> parts,
    std::stop_token stop_token) {
    std::size_t byte_charge = 0;
    for (const auto& part : parts) {
        byte_charge += part.size();
    }
    return submit_filesystem_operation<std::string>(
        impl_->runtime_target,
        impl_->sync,
        byte_charge,
        stop_token,
        std::nullopt,
        [parts = std::move(parts)](const SyncLocalExecutionEnv& sync) {
            return sync.joinPath(parts);
        });
}

support::AsyncResult<std::string, FileError> AsyncLocalExecutionEnv::readTextFile(
    std::string path,
    std::stop_token stop_token) {
    return submit_filesystem_operation<std::string>(
        impl_->runtime_target, impl_->sync, path.size(), stop_token, path,
        [path = std::move(path)](const SyncLocalExecutionEnv& sync) {
            return sync.readTextFile(path);
        });
}

support::AsyncResult<std::vector<std::string>, FileError> AsyncLocalExecutionEnv::readTextLines(
    std::string path,
    std::optional<int> maxLines,
    std::stop_token stop_token) {
    return submit_filesystem_operation<std::vector<std::string>>(
        impl_->runtime_target, impl_->sync, path.size(), stop_token, path,
        [path = std::move(path), maxLines](const SyncLocalExecutionEnv& sync) {
            return sync.readTextLines(path, maxLines);
        });
}

support::AsyncResult<BinaryData, FileError> AsyncLocalExecutionEnv::readBinaryFile(
    std::string path,
    std::stop_token stop_token) {
    return submit_filesystem_operation<BinaryData>(
        impl_->runtime_target, impl_->sync, path.size(), stop_token, path,
        [path = std::move(path)](const SyncLocalExecutionEnv& sync) {
            return sync.readBinaryFile(path);
        });
}

support::AsyncResult<void, FileError> AsyncLocalExecutionEnv::writeFile(
    std::string path,
    WriteContent content,
    std::stop_token stop_token) {
    const auto byte_charge = path.size() + std::visit([](const auto& value) { return value.size(); }, content);
    return submit_filesystem_operation<void>(
        impl_->runtime_target, impl_->sync, byte_charge, stop_token, path,
        [path = std::move(path), content = std::move(content)](const SyncLocalExecutionEnv& sync) {
            return sync.writeFile(path, content);
        });
}

support::AsyncResult<void, FileError> AsyncLocalExecutionEnv::appendFile(
    std::string path,
    WriteContent content,
    std::stop_token stop_token) {
    const auto byte_charge = path.size() + std::visit([](const auto& value) { return value.size(); }, content);
    return submit_filesystem_operation<void>(
        impl_->runtime_target, impl_->sync, byte_charge, stop_token, path,
        [path = std::move(path), content = std::move(content)](const SyncLocalExecutionEnv& sync) {
            return sync.appendFile(path, content);
        });
}

support::AsyncResult<FileInfo, FileError> AsyncLocalExecutionEnv::fileInfo(
    std::string path,
    std::stop_token stop_token) {
    return submit_filesystem_operation<FileInfo>(
        impl_->runtime_target, impl_->sync, path.size(), stop_token, path,
        [path = std::move(path)](const SyncLocalExecutionEnv& sync) {
            return sync.fileInfo(path);
        });
}

support::AsyncResult<std::vector<FileInfo>, FileError> AsyncLocalExecutionEnv::listDir(
    std::string path,
    std::stop_token stop_token) {
    return submit_filesystem_operation<std::vector<FileInfo>>(
        impl_->runtime_target, impl_->sync, path.size(), stop_token, path,
        [path = std::move(path)](const SyncLocalExecutionEnv& sync) {
            return sync.listDir(path);
        });
}

support::AsyncResult<std::string, FileError> AsyncLocalExecutionEnv::canonicalPath(
    std::string path,
    std::stop_token stop_token) {
    return submit_filesystem_operation<std::string>(
        impl_->runtime_target, impl_->sync, path.size(), stop_token, path,
        [path = std::move(path)](const SyncLocalExecutionEnv& sync) {
            return sync.canonicalPath(path);
        });
}

support::AsyncResult<bool, FileError> AsyncLocalExecutionEnv::exists(
    std::string path,
    std::stop_token stop_token) {
    return submit_filesystem_operation<bool>(
        impl_->runtime_target, impl_->sync, path.size(), stop_token, path,
        [path = std::move(path)](const SyncLocalExecutionEnv& sync) {
            return sync.exists(path);
        });
}

support::AsyncResult<void, FileError> AsyncLocalExecutionEnv::createDir(
    std::string path,
    bool recursive,
    std::stop_token stop_token) {
    return submit_filesystem_operation<void>(
        impl_->runtime_target, impl_->sync, path.size(), stop_token, path,
        [path = std::move(path), recursive](const SyncLocalExecutionEnv& sync) {
            return sync.createDir(path, recursive);
        });
}

support::AsyncResult<void, FileError> AsyncLocalExecutionEnv::remove(
    std::string path,
    bool recursive,
    std::stop_token stop_token) {
    return submit_filesystem_operation<void>(
        impl_->runtime_target, impl_->sync, path.size(), stop_token, path,
        [path = std::move(path), recursive](const SyncLocalExecutionEnv& sync) {
            return sync.remove(path, recursive);
        });
}

support::AsyncResult<std::string, FileError> AsyncLocalExecutionEnv::createTempDir(
    std::optional<std::string> prefix,
    std::stop_token stop_token) {
    const auto byte_charge = prefix ? prefix->size() : 0;
    return submit_filesystem_operation<std::string>(
        impl_->runtime_target, impl_->sync, byte_charge, stop_token, std::nullopt,
        [prefix = std::move(prefix)](const SyncLocalExecutionEnv& sync) {
            return sync.createTempDir(prefix);
        });
}

support::AsyncResult<std::string, FileError> AsyncLocalExecutionEnv::createTempFile(
    std::optional<std::string> prefix,
    std::optional<std::string> suffix,
    std::stop_token stop_token) {
    const auto byte_charge = (prefix ? prefix->size() : 0) + (suffix ? suffix->size() : 0);
    return submit_filesystem_operation<std::string>(
        impl_->runtime_target, impl_->sync, byte_charge, stop_token, std::nullopt,
        [prefix = std::move(prefix), suffix = std::move(suffix)](const SyncLocalExecutionEnv& sync) {
            return sync.createTempFile(prefix, suffix);
        });
}

support::AsyncResult<ShellExecResult, ExecutionError> AsyncLocalExecutionEnv::exec(
    std::string command,
    ExecOptions options) {
    if (options.stop_token.stop_requested()) {
        return support::AsyncResult<ShellExecResult, ExecutionError>{
            std::unexpected(ExecutionError{
                .code = ExecutionErrorCode::Aborted,
                .message = "Operation aborted",
            })};
    }

    const auto byte_charge = command.size();
    auto impl = impl_;
    return support::AsyncResult<ShellExecResult, ExecutionError>{
        support::AsyncProducer<ShellExecResult, ExecutionError>{
            [impl = std::move(impl),
             command = std::move(command),
             options = std::move(options),
             byte_charge](support::AsyncCompletion<ShellExecResult, ExecutionError> completion) mutable noexcept {
                std::shared_ptr<ShellOperationState> state;
                // Kept out-of-line across the state allocation so a setup
                // failure can release the admission through the ordered
                // mailbox at its sequence (FIFO) instead of out of band.
                std::optional<RuntimeTarget::Admission> admission;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                bool terminal_posted = false;
                try {
#endif
                    if (options.stop_token.stop_requested()) {
                        completion(std::unexpected(ExecutionError{
                            .code = ExecutionErrorCode::Aborted,
                            .message = "Operation aborted",
                        }));
                        return;
                    }
                    if (!impl->runtime_target) {
                        completion(std::unexpected(busy_execution_error()));
                        return;
                    }
                    admission = impl->runtime_target->try_admit(
                        byte_charge + kAdmittedOperationOverheadBytes);
                    if (!admission) {
                        completion(std::unexpected(busy_execution_error()));
                        return;
                    }
                    // Allocate the shared state before the move-only admission
                    // and completion are installed, so a setup failure can
                    // still deliver the typed failure through the ordered
                    // mailbox at this admission's sequence (FIFO, §5.4).
                    state = std::make_shared<ShellOperationState>();
                    state->install(
                        std::move(*admission),
                        std::move(completion),
                        impl->runtime_target,
                        impl->sync);
                    const bool queued = state->admission->post_worker(
                        [state,
                         command = std::move(command),
                         options = std::move(options)]() mutable noexcept {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                            try {
#endif
                                state->request = state->sync->make_exec_request(
                                    std::move(command), std::move(options));
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                            } catch (...) {
                                state->complete(std::unexpected(unknown_execution_error()));
                                return;
                            }
#endif
                            if (!state->admission->post_loop([state]() noexcept {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                                try {
#endif
                                    if (!state->request || !*state->request) {
                                        state->complete(std::unexpected(
                                            state->request
                                                ? state->request->error()
                                                : unknown_execution_error()));
                                        return;
                                    }
                                    auto runner = state->sync->process_runner();
                                    if (!runner) {
                                        state->complete(std::unexpected(unknown_execution_error()));
                                        return;
                                    }
                                    boost::asio::co_spawn(
                                        state->runtime_target->executor(),
                                        run_shell_process(state, std::move(runner)),
                                        boost::asio::detached);
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                                } catch (...) {
                                    state->complete(std::unexpected(unknown_execution_error()));
                                }
#endif
                            })) {
                                state->complete(std::unexpected(busy_execution_error()));
                            }
                        });
                    if (!queued) {
                        // Same shutdown-race contract as the filesystem path:
                        // the admitted shell operation becomes typed Busy via
                        // the ordered mailbox, never an inline generic failure.
                        std::move(*state->admission).complete(
                            [state]() mutable noexcept {
                                state->completion(std::unexpected(busy_execution_error()));
                            });
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                        terminal_posted = true;
#endif
                    }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                } catch (...) {
                    const auto failure = unknown_execution_error();
                    if (state && !terminal_posted) {
                        state->complete(std::unexpected(failure));
                    } else if (admission) {
                        // Allocation failed after admission: release the byte
                        // charge and deliver the typed failure through the
                        // ordered mailbox at this admission's sequence so the
                        // FIFO head advances (ADR 0040 mailbox ordering).
                        std::move(*admission).complete(
                            [completion = std::move(completion),
                             failure = failure]() mutable noexcept {
                                completion(std::unexpected(failure));
                            });
                    } else {
                        completion(std::unexpected(failure));
                    }
                }
#endif
            }} };
}

} // namespace cch::harness
