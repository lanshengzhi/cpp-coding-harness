#include <cch/agent/harness/LocalExecutionEnv.hpp>

#include "ExecutionErrorClassification.hpp"
#include "RuntimeRoot.hpp"
#include "SyncLocalExecutionEnv.hpp"
#include "ai/AsyncResultBridge.hpp"
#include "harness/Process.hpp"

#include <boost/asio/co_spawn.hpp>

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
    RuntimeTarget::Admission admission;
    support::AsyncCompletion<T, FileError> completion;
    std::optional<std::expected<T, FileError>> outcome;

    FileOperationState(
        RuntimeTarget::Admission admission,
        support::AsyncCompletion<T, FileError> completion)
        : admission(std::move(admission)), completion(std::move(completion)) {}
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
                try {
                    if (stop_token.stop_requested()) {
                        completion(std::unexpected(aborted_file_error(std::move(path))));
                        return;
                    }
                    if (!runtime_target) {
                        completion(std::unexpected(busy_file_error(std::move(path))));
                        return;
                    }
                    auto admission =
                        runtime_target->try_admit(byte_charge + kAdmittedOperationOverheadBytes);
                    if (!admission) {
                        completion(std::unexpected(busy_file_error(std::move(path))));
                        return;
                    }
                    auto state = std::make_shared<FileOperationState<T>>(
                        std::move(*admission), std::move(completion));
                    const bool queued = state->admission.post_worker(
                        [state,
                         sync = std::move(sync),
                         stop_token,
                         path = std::move(path),
                         operation = std::move(operation)]() mutable noexcept {
                            try {
                                if (stop_token.stop_requested()) {
                                    state->outcome = std::unexpected(aborted_file_error(std::move(path)));
                                } else {
                                    state->outcome = operation(*sync);
                                    if (stop_token.stop_requested()) {
                                        state->outcome = std::unexpected(aborted_file_error(
                                            std::move(path), true));
                                    }
                                }
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
                            std::move(state->admission).complete(
                                [state]() mutable noexcept {
                                    state->completion(std::move(*state->outcome));
                                });
                        });
                    if (!queued) {
                        // Admission succeeded but the worker queue is already
                        // stopping: deliver a typed Busy through the ordered
                        // mailbox so the admission is released in sequence.
                        std::move(state->admission).complete(
                            [state, path = std::move(path)]() mutable noexcept {
                                state->completion(std::unexpected(
                                    busy_file_error(std::move(path))));
                            });
                    }
                } catch (const std::exception& error) {
                    completion(std::unexpected(FileError{
                        .code = FileErrorCode::Unknown,
                        .message = error.what(),
                        .path = std::move(path),
                    }));
                } catch (...) {
                    completion(std::unexpected(FileError{
                        .code = FileErrorCode::Unknown,
                        .message = "filesystem operation could not be scheduled",
                        .path = std::move(path),
                    }));
                }
            }} };
}

struct ShellOperationState final : std::enable_shared_from_this<ShellOperationState> {
    RuntimeTarget::Admission admission;
    support::AsyncCompletion<ShellExecResult, ExecutionError> completion;
    std::shared_ptr<RuntimeTarget> runtime_target;
    std::shared_ptr<SyncLocalExecutionEnv> sync;
    std::optional<std::expected<harness::ProcessRequest, ExecutionError>> request;

    ShellOperationState(
        RuntimeTarget::Admission admission,
        support::AsyncCompletion<ShellExecResult, ExecutionError> completion,
        std::shared_ptr<RuntimeTarget> runtime_target,
        std::shared_ptr<SyncLocalExecutionEnv> sync)
        : admission(std::move(admission)),
          completion(std::move(completion)),
          runtime_target(std::move(runtime_target)),
          sync(std::move(sync)) {}

    void complete(std::expected<ShellExecResult, ExecutionError> outcome) noexcept {
        std::move(admission).complete(
            [self = shared_from_this(), outcome = std::move(outcome)]() mutable noexcept {
                self->completion(std::move(outcome));
            });
    }

};

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
            std::unexpected(ExecutionError{ExecutionErrorCode::Aborted, "Operation aborted"})};
    }

    const auto byte_charge = command.size();
    auto impl = impl_;
    return support::AsyncResult<ShellExecResult, ExecutionError>{
        support::AsyncProducer<ShellExecResult, ExecutionError>{
            [impl = std::move(impl),
             command = std::move(command),
             options = std::move(options),
             byte_charge](support::AsyncCompletion<ShellExecResult, ExecutionError> completion) mutable noexcept {
                try {
                    if (options.stop_token.stop_requested()) {
                        completion(std::unexpected(ExecutionError{
                            ExecutionErrorCode::Aborted, "Operation aborted"}));
                        return;
                    }
                    if (!impl->runtime_target) {
                        completion(std::unexpected(busy_execution_error()));
                        return;
                    }
                    auto admission = impl->runtime_target->try_admit(
                        byte_charge + kAdmittedOperationOverheadBytes);
                    if (!admission) {
                        completion(std::unexpected(busy_execution_error()));
                        return;
                    }
                    auto state = std::make_shared<ShellOperationState>(
                        std::move(*admission),
                        std::move(completion),
                        impl->runtime_target,
                        impl->sync);
                    const bool queued = state->admission.post_worker(
                        [state,
                         command = std::move(command),
                         options = std::move(options)]() mutable noexcept {
                            try {
                                state->request = state->sync->make_exec_request(
                                    std::move(command), std::move(options));
                            } catch (...) {
                                state->request = std::unexpected(unknown_execution_error());
                            }
                            if (!state->admission.post_loop([state]() noexcept {
                                    if (!state->request || !*state->request) {
                                        state->complete(std::unexpected(
                                            state->request
                                                ? state->request->error()
                                                : unknown_execution_error()));
                                        return;
                                    }
                                    try {
                                        auto runner = state->sync->process_runner();
                                        boost::asio::co_spawn(
                                            state->runtime_target->executor(),
                                            runner->run(std::move(**state->request)),
                                            [state](
                                                std::exception_ptr exception,
                                                support::Expected<harness::ProcessResult> process) mutable noexcept {
                                                if (exception) {
                                                    state->complete(std::unexpected(unknown_execution_error()));
                                                } else if (!process) {
                                                    state->complete(std::unexpected(
                                                        classify_terminal_process_error(process.error())));
                                                } else if (process->timed_out) {
                                                    state->complete(std::unexpected(ExecutionError{
                                                        ExecutionErrorCode::Timeout,
                                                        "shell command timed out"}));
                                                } else {
                                                    state->complete(state->sync->exec_result_from_process(*process));
                                                }
                                            });
                                    } catch (...) {
                                        state->complete(std::unexpected(unknown_execution_error()));
                                    }
                                })) {
                                state->complete(std::unexpected(busy_execution_error()));
                            }
                        });
                    if (!queued) {
                        // Same shutdown-race contract as the filesystem path:
                        // the admitted shell operation becomes typed Busy via
                        // the ordered mailbox, never an inline generic failure.
                        std::move(state->admission).complete(
                            [state]() mutable noexcept {
                                state->completion(std::unexpected(busy_execution_error()));
                            });
                    }
                } catch (...) {
                    completion(std::unexpected(unknown_execution_error()));
                }
            }} };
}

} // namespace cch::harness
