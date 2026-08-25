#include "SyncLocalExecutionEnv.hpp"

#include "ExecutionErrorClassification.hpp"
#include "support/AsyncResultBridge.hpp"
#include "ShellEnvironment.hpp"
#include "ShellResolver.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <exception>
#include <memory>
#include <optional>
#include <utility>

namespace cch::harness {
namespace {

struct SyncProcessOperation final {
    std::shared_ptr<harness::AsyncProcessRunner> runner;
    harness::ProcessRequest request;
    std::optional<support::Expected<harness::ProcessResult>> outcome{std::nullopt};

    [[nodiscard]] boost::asio::awaitable<support::Expected<harness::ProcessResult>> operator()() {
        return runner->run(std::move(request));
    }
};

[[nodiscard]] boost::asio::awaitable<void> run_sync_process(std::shared_ptr<SyncProcessOperation> operation) {
    auto process =
            co_await support::detail::invoke_awaitable<SyncProcessOperation, support::Expected<harness::ProcessResult>>(
                    operation);
    operation->outcome = std::move(process);
    co_return;
}

} // namespace

SyncLocalExecutionEnv::SyncLocalExecutionEnv(
    std::filesystem::path workspace,
    bool bash_enabled,
    std::vector<std::string> secret_environment_names,
    ShellConfig shell_config,
    std::shared_ptr<harness::AsyncProcessRunner> runner)
    : workspace_(std::move(workspace)),
      bash_enabled_(bash_enabled),
      secret_environment_names_(std::move(secret_environment_names)),
      shell_config_(std::move(shell_config)),
      runner_(std::move(runner)),
      fs_(workspace_) {}

// ---------------------------------------------------------------------------
// Pi-shaped filesystem methods
// ---------------------------------------------------------------------------

std::expected<std::string, FileError> SyncLocalExecutionEnv::absolutePath(const std::string& path) const {
    return fs_.absolutePath(path);
}

std::expected<std::string, FileError> SyncLocalExecutionEnv::joinPath(const std::vector<std::string>& parts) const {
    return fs_.joinPath(parts);
}

std::expected<std::string, FileError> SyncLocalExecutionEnv::readTextFile(const std::string& path) const {
    return fs_.readTextFile(path);
}

std::expected<std::vector<std::string>, FileError> SyncLocalExecutionEnv::readTextLines(
    const std::string& path,
    std::optional<int> maxLines) const {
    return fs_.readTextLines(path, maxLines);
}

std::expected<BinaryData, FileError> SyncLocalExecutionEnv::readBinaryFile(const std::string& path) const {
    return fs_.readBinaryFile(path);
}

std::expected<void, FileError> SyncLocalExecutionEnv::writeFile(const std::string& path, const WriteContent& content) const {
    return fs_.writeFile(path, content);
}

std::expected<void, FileError> SyncLocalExecutionEnv::appendFile(const std::string& path, const WriteContent& content) const {
    return fs_.appendFile(path, content);
}

std::expected<FileInfo, FileError> SyncLocalExecutionEnv::fileInfo(const std::string& path) const {
    return fs_.fileInfo(path);
}

std::expected<std::vector<FileInfo>, FileError> SyncLocalExecutionEnv::listDir(const std::string& path) const {
    return fs_.listDir(path);
}

std::expected<std::string, FileError> SyncLocalExecutionEnv::canonicalPath(const std::string& path) const {
    return fs_.canonicalPath(path);
}

std::expected<bool, FileError> SyncLocalExecutionEnv::exists(const std::string& path) const {
    return fs_.exists(path);
}

std::expected<void, FileError> SyncLocalExecutionEnv::createDir(const std::string& path, bool recursive) const {
    return fs_.createDir(path, recursive);
}

std::expected<void, FileError> SyncLocalExecutionEnv::remove(const std::string& path, bool recursive) const {
    return fs_.remove(path, recursive);
}

std::expected<std::string, FileError> SyncLocalExecutionEnv::createTempDir(std::optional<std::string> prefix) const {
    return fs_.createTempDir(prefix);
}

std::expected<std::string, FileError> SyncLocalExecutionEnv::createTempFile(
    std::optional<std::string> prefix,
    std::optional<std::string> suffix) const {
    return fs_.createTempFile(prefix, suffix);
}

// ---------------------------------------------------------------------------
// Pi-shaped shell methods
// ---------------------------------------------------------------------------

std::expected<harness::ProcessRequest, ExecutionError> SyncLocalExecutionEnv::make_exec_request(
    std::string command,
    ExecOptions options) const {
    if (options.stop_token.stop_requested()) {
        return std::unexpected(ExecutionError{
            .code = ExecutionErrorCode::Aborted,
            .message = "Operation aborted",
        });
    }
    if (!bash_enabled_) {
        return std::unexpected(ExecutionError{
            .code = ExecutionErrorCode::ShellUnavailable,
            .message = "bash is disabled by default; rerun with explicit bash enablement",
        });
    }

    // Validate cwd override through workspace containment.
    std::filesystem::path working_dir = workspace_;
    if (options.cwd) {
        auto resolved = fs_.resolve_addressed_path(*options.cwd);
        if (!resolved) {
            return std::unexpected(ExecutionError{
                .code = ExecutionErrorCode::SpawnError,
                .message = resolved.error().detail,
            });
        }
        std::error_code error;
        const auto status = std::filesystem::symlink_status(*resolved, error);
        if (error || !std::filesystem::is_directory(status)) {
            return std::unexpected(ExecutionError{
                .code = ExecutionErrorCode::SpawnError,
                .message = "cwd does not exist or is not a directory: " + *options.cwd,
            });
        }
        working_dir = *resolved;
    }

    // Build sanitized base environment.
    auto base_env = sanitized_environment(secret_environment_names_);

    // Apply explicit overrides, stripping secret-like names.
    if (options.env) {
        for (const auto& [key, value] : *options.env) {
            if (secret_env_name(key, secret_environment_names_)) {
                continue;
            }
            base_env[key] = value;
        }
    }

    std::filesystem::path executable;
    if (auto shell = resolve_shell_executable(
            shell_config_.shell_path,
            workspace_,
            base_env);
        !shell) {
        return std::unexpected(shell.error());
    } else {
        executable = std::move(*shell);
    }

    std::string script = std::move(command);
    if (shell_config_.command_prefix && !shell_config_.command_prefix->empty()) {
        script = *shell_config_.command_prefix + "\n" + script;
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

ShellExecResult SyncLocalExecutionEnv::exec_result_from_process(const harness::ProcessResult& process) const {
    ShellExecResult result;
    result.stdout_output = process.stdout_output;
    result.stderr_output = process.stderr_output;
    result.exitCode = process.exit_code;
    return result;
}

std::expected<ShellExecResult, ExecutionError> SyncLocalExecutionEnv::exec(
    std::string command,
    ExecOptions options) const {
    auto request = make_exec_request(std::move(command), std::move(options));
    if (!request) {
        return std::unexpected(request.error());
    }

    boost::asio::io_context io;
    std::shared_ptr<SyncProcessOperation> operation;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        operation = std::make_shared<SyncProcessOperation>(SyncProcessOperation{
            .runner = runner_,
            .request = std::move(*request),
        });
        boost::asio::co_spawn(
            io,
            run_sync_process(operation),
            boost::asio::detached);
        io.run();
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& error) {
        return std::unexpected(ExecutionError{
            .code = ExecutionErrorCode::SpawnError,
            .message = "process execution could not start: " + std::string{error.what()},
        });
    } catch (...) {
        return std::unexpected(ExecutionError{
            .code = ExecutionErrorCode::SpawnError,
            .message = "process execution could not start",
        });
    }
#endif

    if (!operation || !operation->outcome) {
        return std::unexpected(ExecutionError{
            .code = ExecutionErrorCode::SpawnError,
            .message = "process execution did not complete",
        });
    }
    if (!*operation->outcome) {
        return std::unexpected(classify_process_execution_error(operation->outcome->error()));
    }
    if ((*operation->outcome)->timed_out) {
        return std::unexpected(ExecutionError{
            .code = ExecutionErrorCode::Timeout,
            .message = "shell command timed out",
        });
    }
    return exec_result_from_process(**operation->outcome);
}

} // namespace cch::harness
