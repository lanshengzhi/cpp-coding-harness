#include <cch/agent/harness/LocalShell.hpp>

#include "AsyncFileSystemOperations.hpp"
#include "ExecutionErrorClassification.hpp"
#include "RuntimeRoot.hpp"
#include "ShellEnvironment.hpp"
#include "ShellResolver.hpp"
#include "WorkspaceFileSystem.hpp"
#include "support/AsyncResultBridge.hpp"
#include "agent/harness/Process.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>

#include <exception>
#include <memory>
#include <optional>
#include <utility>

namespace cch::harness {

struct AsyncLocalShell::Impl final {
    Impl(std::shared_ptr<RuntimeTarget> runtime_target,
            std::filesystem::path workspace,
            bool bash_enabled,
            std::vector<std::string> secret_environment_names,
            ShellConfig shell_config,
            std::shared_ptr<harness::AsyncProcessRunner> runner)
        : runtime_target(std::move(runtime_target)), filesystem(std::move(workspace)), bash_enabled(bash_enabled),
          secret_environment_names(std::move(secret_environment_names)), shell_config(std::move(shell_config)),
          runner(std::move(runner)) {}

    [[nodiscard]] std::expected<harness::ProcessRequest, ExecutionError> make_exec_request(
            std::string command, ExecOptions options) const;

    std::shared_ptr<RuntimeTarget> runtime_target;
    WorkspaceFileSystem filesystem;
    bool bash_enabled;
    std::vector<std::string> secret_environment_names;
    ShellConfig shell_config;
    std::shared_ptr<harness::AsyncProcessRunner> runner;
};

namespace {

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

[[nodiscard]] ShellExecResult exec_result_from_process(const harness::ProcessResult& process) {
    ShellExecResult result;
    result.stdout_output = process.stdout_output;
    result.stderr_output = process.stderr_output;
    result.exitCode = process.exit_code;
    return result;
}

struct ShellOperationState final : std::enable_shared_from_this<ShellOperationState> {
    std::optional<RuntimeTarget::Admission> admission;
    support::AsyncCompletion<ShellExecResult, ExecutionError> completion;
    std::shared_ptr<RuntimeTarget> runtime_target;
    std::optional<std::expected<harness::ProcessRequest, ExecutionError>> request;

    /// Install the move-only admission/completion and owned references only
    /// after the shared allocation succeeds, so a setup failure can still
    /// deliver the typed failure through the ordered mailbox at this
    /// admission's sequence instead of losing the completion and wedging the
    /// FIFO head (§5.4 / ADR 0040). Must not be called twice.
    void install(RuntimeTarget::Admission admission,
            support::AsyncCompletion<ShellExecResult, ExecutionError> completion,
            std::shared_ptr<RuntimeTarget> runtime_target) noexcept {
        this->admission = std::move(admission);
        this->completion = std::move(completion);
        this->runtime_target = std::move(runtime_target);
    }

    void complete(std::expected<ShellExecResult, ExecutionError> outcome) noexcept {
        std::move(*admission).complete([self = shared_from_this(), outcome = std::move(outcome)]() mutable noexcept {
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
        std::shared_ptr<ShellOperationState> state, std::shared_ptr<AsyncProcessRunner> runner) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        auto operation = std::make_shared<ShellProcessOperation>(ShellProcessOperation{
                .runner = std::move(runner),
                .request = std::move(**state->request),
        });
        auto process = co_await support::detail::invoke_awaitable<ShellProcessOperation,
                support::Expected<harness::ProcessResult>>(std::move(operation));
        if (!process) {
            state->complete(std::unexpected(classify_terminal_process_error(process.error())));
        } else if (process->timed_out) {
            state->complete(std::unexpected(ExecutionError{
                    .code = ExecutionErrorCode::Timeout,
                    .message = "shell command timed out",
            }));
        } else {
            state->complete(exec_result_from_process(*process));
        }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (...) {
        state->complete(std::unexpected(unknown_execution_error()));
    }
#endif
    co_return;
}

} // namespace

std::expected<harness::ProcessRequest, ExecutionError> AsyncLocalShell::Impl::make_exec_request(
        std::string command, ExecOptions options) const {
    if (options.stop_token.stop_requested()) {
        return std::unexpected(ExecutionError{
                .code = ExecutionErrorCode::Aborted,
                .message = "Operation aborted",
        });
    }
    if (!bash_enabled) {
        return std::unexpected(ExecutionError{
                .code = ExecutionErrorCode::ShellUnavailable,
                .message = "bash is disabled by default; rerun with explicit bash enablement",
        });
    }

    // Validate cwd override through workspace containment.
    std::filesystem::path working_dir = filesystem.root();
    if (options.cwd) {
        auto resolved = filesystem.resolve_addressed_path(*options.cwd);
        if (!resolved) {
            return std::unexpected(ExecutionError{
                    .code = ExecutionErrorCode::SpawnError,
                    .message = resolved.error().detail,
            });
        }
        if (auto validated = filesystem.validate_directory(*resolved); !validated) {
            return std::unexpected(ExecutionError{
                    .code = ExecutionErrorCode::SpawnError,
                    .message = validated.error().detail,
            });
        }
        working_dir = *resolved;
    }

    // Build sanitized base environment.
    auto base_env = sanitized_environment(secret_environment_names);

    // Apply explicit overrides, stripping secret-like names.
    if (options.env) {
        for (const auto& [key, value] : *options.env) {
            if (secret_env_name(key, secret_environment_names)) {
                continue;
            }
            base_env[key] = value;
        }
    }

    std::filesystem::path executable;
    if (auto shell = resolve_shell_executable(shell_config.shell_path, filesystem.root(), base_env); !shell) {
        return std::unexpected(shell.error());
    } else {
        executable = std::move(*shell);
    }

    std::string script = std::move(command);
    if (shell_config.command_prefix && !shell_config.command_prefix->empty()) {
        script = *shell_config.command_prefix + "\n" + script;
    }

    harness::ProcessRequest request;
    request.executable = std::move(executable);
    request.arguments = {"-c", std::move(script)};
    request.stop_token = options.stop_token;
    request.working_directory = working_dir;
    request.timeout = options.timeout.value_or(std::chrono::milliseconds{30000});
    request.environment = std::move(base_env);
    request.use_explicit_environment = true;

    // Move callbacks if provided.
    if (options.onStdout) {
        request.on_stdout = std::move(*options.onStdout);
    }
    if (options.onStderr) {
        request.on_stderr = std::move(*options.onStderr);
    }

    return request;
}

AsyncLocalShell::AsyncLocalShell(std::shared_ptr<RuntimeTarget> runtime_target,
        std::filesystem::path workspace,
        bool bash_enabled,
        std::vector<std::string> secret_environment_names,
        ShellConfig shell_config)
    : AsyncLocalShell(std::move(runtime_target),
              std::move(workspace),
              bash_enabled,
              std::move(secret_environment_names),
              std::move(shell_config),
              std::make_shared<harness::DefaultAsyncProcessRunner>()) {}

AsyncLocalShell::AsyncLocalShell(std::shared_ptr<RuntimeTarget> runtime_target,
        std::filesystem::path workspace,
        bool bash_enabled,
        std::vector<std::string> secret_environment_names,
        ShellConfig shell_config,
        std::shared_ptr<harness::AsyncProcessRunner> runner)
    : impl_(std::make_shared<Impl>(std::move(runtime_target),
              std::move(workspace),
              bash_enabled,
              std::move(secret_environment_names),
              std::move(shell_config),
              std::move(runner))) {}

AsyncLocalShell::AsyncLocalShell(AsyncLocalShell&&) noexcept = default;
AsyncLocalShell& AsyncLocalShell::operator=(AsyncLocalShell&&) noexcept = default;
AsyncLocalShell::~AsyncLocalShell() = default;

support::AsyncResult<ShellExecResult, ExecutionError> AsyncLocalShell::exec(std::string command, ExecOptions options) {
    if (options.stop_token.stop_requested()) {
        return support::AsyncResult<ShellExecResult, ExecutionError>{std::unexpected(ExecutionError{
                .code = ExecutionErrorCode::Aborted,
                .message = "Operation aborted",
        })};
    }

    const auto byte_charge = command.size();
    auto impl = impl_;
    return support::AsyncResult<ShellExecResult, ExecutionError>{
            support::AsyncProducer<ShellExecResult, ExecutionError>{
                    [impl = std::move(impl), command = std::move(command), options = std::move(options), byte_charge](
                            support::AsyncCompletion<ShellExecResult, ExecutionError> completion) mutable noexcept {
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
                            admission = impl->runtime_target->try_admit(filesystem_detail::saturating_add(
                                    byte_charge, filesystem_detail::kAdmittedOperationOverheadBytes));
                            if (!admission) {
                                completion(std::unexpected(busy_execution_error()));
                                return;
                            }
                            // Allocate the shared state before the move-only admission
                            // and completion are installed, so a setup failure can
                            // still deliver the typed failure through the ordered
                            // mailbox at this admission's sequence (FIFO, §5.4).
                            state = std::make_shared<ShellOperationState>();
                            state->install(std::move(*admission), std::move(completion), impl->runtime_target);
                            const bool queued = state->admission->post_worker(
                                    [state,
                                            impl,
                                            command = std::move(command),
                                            options = std::move(options)]() mutable noexcept {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                                        try {
#endif
                                            state->request =
                                                    impl->make_exec_request(std::move(command), std::move(options));
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                                        } catch (...) {
                                            state->complete(std::unexpected(unknown_execution_error()));
                                            return;
                                        }
#endif
                                        if (!state->admission->post_loop([state, impl]() noexcept {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                                                try {
#endif
                                                    if (!state->request || !*state->request) {
                                                        state->complete(std::unexpected(
                                                                state->request ? state->request->error()
                                                                               : unknown_execution_error()));
                                                        return;
                                                    }
                                                    auto runner = impl->runner;
                                                    if (!runner) {
                                                        state->complete(std::unexpected(unknown_execution_error()));
                                                        return;
                                                    }
                                                    boost::asio::co_spawn(state->runtime_target->executor(),
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
                                std::move(*state->admission).complete([state]() mutable noexcept {
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
                                std::move(*admission)
                                        .complete([completion = std::move(completion),
                                                          failure = failure]() mutable noexcept {
                                            completion(std::unexpected(failure));
                                        });
                            } else {
                                completion(std::unexpected(failure));
                            }
                        }
#endif
                    }}};
}

} // namespace cch::harness
