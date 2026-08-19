#include "LocalUserShell.hpp"

#include "ai/AsyncResultBridge.hpp"
#include "agent/harness/ShellEnvironment.hpp"
#include "agent/harness/ShellResolver.hpp"
#include "agent/harness/Process.hpp"

#include <chrono>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

namespace cch::coding_agent::runtime {
namespace {

[[nodiscard]] boost::asio::awaitable<support::Expected<UserShellResult>> run_local_user_shell(
    std::filesystem::path workspace,
    std::vector<std::string> secret_environment_names,
    harness::ShellConfig shell_config,
    std::string command,
    UserShellUpdateSink update_sink,
    std::stop_token stop_token) {
    auto environment = harness::sanitized_environment(secret_environment_names);
    auto shell = harness::resolve_shell_executable(
        shell_config.shell_path,
        workspace,
        environment);
    if (!shell) {
        co_return std::unexpected(harness::to_util_error(std::move(shell.error())));
    }

    std::string script = std::move(command);
    if (shell_config.command_prefix && !shell_config.command_prefix->empty()) {
        script = *shell_config.command_prefix + "\n" + script;
    }

    // An update-sink failure is an infrastructure failure: stop the process
    // promptly instead of streaming into a dead presentation.
    std::stop_source cancel_source;
    const std::stop_callback forward_cancellation{
        stop_token,
        [&cancel_source] { cancel_source.request_stop(); }};
    std::optional<support::Error> sink_error;

    harness::ProcessRequest request;
    request.executable = std::move(*shell);
    request.arguments = {"-c", std::move(script)};
    request.working_directory = std::move(workspace);
    // User Bash has no default timeout; zero disables the runner deadline.
    request.timeout = std::chrono::milliseconds{0};
    request.environment = std::move(environment);
    request.use_explicit_environment = true;
    request.stop_token = cancel_source.get_token();
    request.merge_stderr = true;
    if (update_sink) {
        request.on_stdout.emplace(
            // The process runner owns this callback only until the awaited run
            // completes; these references point into this coroutine frame.
            [&update_sink, &sink_error, &cancel_source](std::string_view chunk)
                -> support::ExpectedVoid {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                try {
#endif
                    if (sink_error) {
                        return {};
                    }
                    if (auto delivered = update_sink(chunk); !delivered) {
                        sink_error = delivered.error();
                        cancel_source.request_stop();
                        return std::unexpected(std::move(delivered.error()));
                    }
                    return {};
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                } catch (...) {
                    sink_error = support::make_error(
                        support::ErrorCode::Unknown,
                        "user shell update sink threw");
                    cancel_source.request_stop();
                    return std::unexpected(*sink_error);
                }
#endif
            });
    }

    harness::DefaultAsyncProcessRunner runner;
    auto process = co_await runner.run(std::move(request));

    if (sink_error) {
        co_return std::unexpected(std::move(*sink_error));
    }
    if (!process) {
        if (process.error().code == support::ErrorCode::Cancelled) {
            co_return UserShellResult{.cancelled = true};
        }
        co_return std::unexpected(std::move(process.error()));
    }
    co_return UserShellResult{.exit_code = process->exit_code};
}

} // namespace

LocalUserShell::LocalUserShell(
    std::filesystem::path workspace,
    std::vector<std::string> secret_environment_names,
    harness::ShellConfig shell_config)
    : workspace_(std::move(workspace)),
      secret_environment_names_(std::move(secret_environment_names)),
      shell_config_(std::move(shell_config)) {}

support::AsyncResult<UserShellResult> LocalUserShell::execute(
    std::string command,
    UserShellUpdateSink update_sink,
    std::stop_token stop_token) {
    return ai::detail::make_async_result(
        [workspace = workspace_,
         secret_environment_names = secret_environment_names_,
         shell_config = shell_config_,
         command = std::move(command),
         update_sink = std::move(update_sink),
         stop_token]() mutable {
            return run_local_user_shell(
                std::move(workspace),
                std::move(secret_environment_names),
                std::move(shell_config),
                std::move(command),
                std::move(update_sink),
                stop_token);
        });
}

} // namespace cch::coding_agent::runtime
