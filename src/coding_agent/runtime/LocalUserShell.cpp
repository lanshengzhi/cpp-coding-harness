#include "LocalUserShell.hpp"

#include "harness/ShellEnvironment.hpp"
#include "harness/ShellResolver.hpp"
#include "util/Process.hpp"

#include <chrono>
#include <exception>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

namespace cch::coding_agent::runtime {

LocalUserShell::LocalUserShell(
    std::filesystem::path workspace,
    std::vector<std::string> secret_environment_names,
    harness::ShellConfig shell_config)
    : workspace_(std::move(workspace)),
      secret_environment_names_(std::move(secret_environment_names)),
      shell_config_(std::move(shell_config)) {}

boost::asio::awaitable<util::Expected<UserShellResult>> LocalUserShell::execute(
    std::string command,
    UserShellUpdateSink update_sink,
    std::stop_token stop_token) {
    auto environment = harness::sanitized_environment(secret_environment_names_);
    auto shell = harness::resolve_shell_executable(
        shell_config_.shell_path,
        workspace_,
        environment);
    if (!shell) {
        co_return std::unexpected(harness::to_util_error(std::move(shell.error())));
    }

    std::string script = std::move(command);
    if (shell_config_.command_prefix && !shell_config_.command_prefix->empty()) {
        script = *shell_config_.command_prefix + "\n" + script;
    }

    // An update-sink failure is an infrastructure failure: stop the process
    // promptly instead of streaming into a dead presentation.
    std::stop_source cancel_source;
    const std::stop_callback forward_cancellation{
        stop_token,
        [&cancel_source] { cancel_source.request_stop(); }};
    std::optional<util::Error> sink_error;

    util::ProcessRequest request;
    request.executable = std::move(*shell);
    request.arguments = {"-c", std::move(script)};
    request.working_directory = workspace_;
    // User Bash has no default timeout; zero disables the runner deadline.
    request.timeout = std::chrono::milliseconds{0};
    request.environment = std::move(environment);
    request.use_explicit_environment = true;
    request.stop_token = cancel_source.get_token();
    request.merge_stderr = true;
    if (update_sink) {
        request.on_stdout.emplace(
            [&update_sink, &sink_error, &cancel_source](std::string_view chunk) {
                if (sink_error) {
                    return;
                }
                try {
                    if (auto delivered = update_sink(chunk); !delivered) {
                        sink_error = delivered.error();
                        cancel_source.request_stop();
                    }
                } catch (...) {
                    sink_error = util::make_error(
                        util::ErrorCode::Unknown,
                        "User Shell update sink failed");
                    cancel_source.request_stop();
                }
            });
    }

    util::DefaultAsyncProcessRunner runner;
    auto process = co_await runner.run(std::move(request));

    if (sink_error) {
        co_return std::unexpected(std::move(*sink_error));
    }
    if (!process) {
        if (process.error().code == util::ErrorCode::Cancelled) {
            co_return UserShellResult{.cancelled = true};
        }
        co_return std::unexpected(std::move(process.error()));
    }
    co_return UserShellResult{.exit_code = process->exit_code};
}

} // namespace cch::coding_agent::runtime
