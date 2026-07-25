#include "SyncLocalExecutionEnv.hpp"

#include "ExecutionErrorClassification.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
extern char** environ;
#endif

namespace cch::harness {
namespace {

bool secret_env_name(std::string name, const std::vector<std::string>& explicit_secret_names = {}) {
    for (const auto& explicit_name : explicit_secret_names) {
        if (name == explicit_name) {
            return true;
        }
    }
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return name.find("API_KEY") != std::string::npos || name.find("TOKEN") != std::string::npos ||
           name.find("SECRET") != std::string::npos || name.find("PASSWORD") != std::string::npos ||
           name.find("CREDENTIAL") != std::string::npos || name.find("PRIVATE_KEY") != std::string::npos ||
           name.find("AUTH") != std::string::npos || name.find("JWT") != std::string::npos ||
           name.find("CERTIFICATE") != std::string::npos || name.find("PASSPHRASE") != std::string::npos ||
           name.find("OPENAI") != std::string::npos;
}

std::map<std::string, std::string> sanitized_environment(const std::vector<std::string>& explicit_secret_names = {}) {
    std::map<std::string, std::string> env;
#if defined(__unix__) || defined(__APPLE__)
    for (char** current = environ; current != nullptr && *current != nullptr; ++current) {
        std::string entry(*current);
        auto split = entry.find('=');
        if (split == std::string::npos) {
            continue;
        }
        auto key = entry.substr(0, split);
        if (!secret_env_name(key, explicit_secret_names)) {
            env[key] = entry.substr(split + 1);
        }
    }
#else
    (void)explicit_secret_names;
#endif
    return env;
}

[[nodiscard]] util::Error workspace_error(std::string message) {
    return util::make_error(util::ErrorCode::Workspace, message, message);
}

[[nodiscard]] util::Error process_error(std::string message) {
    return util::make_error(util::ErrorCode::Process, message, message);
}

} // namespace

SyncLocalExecutionEnv::SyncLocalExecutionEnv(
    std::filesystem::path workspace,
    bool bash_enabled,
    std::vector<std::string> secret_environment_names,
    std::shared_ptr<util::AsyncProcessRunner> runner)
    : workspace_(std::move(workspace)),
      bash_enabled_(bash_enabled),
      secret_environment_names_(std::move(secret_environment_names)),
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

util::Expected<util::ProcessRequest> SyncLocalExecutionEnv::make_exec_request(
    std::string command,
    ExecOptions options) const {
    if (!bash_enabled_) {
        return std::unexpected(process_error("bash is disabled by default; rerun with explicit bash enablement"));
    }

    // Validate cwd override through workspace containment.
    std::filesystem::path working_dir = workspace_;
    if (options.cwd) {
        auto resolved = fs_.resolve_addressed_path(*options.cwd);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        std::error_code ec;
        auto status = std::filesystem::symlink_status(*resolved, ec);
        if (ec || !std::filesystem::is_directory(status)) {
            return std::unexpected(workspace_error("cwd does not exist or is not a directory: " + *options.cwd));
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

    util::ProcessRequest request;
    request.command = std::move(command);
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

ShellExecResult SyncLocalExecutionEnv::exec_result_from_process(const util::ProcessResult& process) const {
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
        return std::unexpected(classify_execution_error(
            request.error(), ExecutionErrorOrigin::Request));
    }

    boost::asio::io_context io;
    std::optional<util::Expected<util::ProcessResult>> process;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            process = co_await runner_->run(std::move(*request));
            co_return;
        },
        boost::asio::detached);
    io.run();

    if (!process) {
        return std::unexpected(ExecutionError{ExecutionErrorCode::SpawnError, "process execution did not complete"});
    }
    if (!*process) {
        return std::unexpected(classify_execution_error(
            (*process).error(), ExecutionErrorOrigin::Process));
    }
    if ((*process)->timed_out) {
        return std::unexpected(ExecutionError{ExecutionErrorCode::Timeout, "shell command timed out"});
    }
    return exec_result_from_process(**process);
}

} // namespace cch::harness
