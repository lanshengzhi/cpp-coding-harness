#include <cch/harness/LocalExecutionEnv.hpp>

#include "ExecutionErrorClassification.hpp"
#include "SyncLocalExecutionEnv.hpp"
#include "util/Process.hpp"

#include <utility>

namespace cch::harness {
namespace {

[[nodiscard]] FileError aborted_file_error(std::optional<std::string> path = std::nullopt) {
    return FileError{
        .code = FileErrorCode::Aborted,
        .message = "Operation aborted",
        .path = std::move(path),
    };
}

template <typename Operation>
[[nodiscard]] auto observe_filesystem_cancellation(
    std::stop_token stop_token,
    std::optional<std::string> path,
    Operation operation) -> decltype(operation()) {
    if (stop_token.stop_requested()) {
        return std::unexpected(aborted_file_error(std::move(path)));
    }
    auto result = operation();
    if (stop_token.stop_requested()) {
        return std::unexpected(aborted_file_error(std::move(path)));
    }
    return result;
}

} // namespace

AsyncLocalExecutionEnv::AsyncLocalExecutionEnv(
    std::filesystem::path workspace,
    bool bash_enabled,
    std::vector<std::string> secret_environment_names)
    : sync_(std::make_unique<SyncLocalExecutionEnv>(
        std::move(workspace),
        bash_enabled,
        std::move(secret_environment_names))) {}

AsyncLocalExecutionEnv::AsyncLocalExecutionEnv(AsyncLocalExecutionEnv&&) noexcept = default;
AsyncLocalExecutionEnv& AsyncLocalExecutionEnv::operator=(AsyncLocalExecutionEnv&&) noexcept = default;
AsyncLocalExecutionEnv::~AsyncLocalExecutionEnv() = default;

const std::filesystem::path& AsyncLocalExecutionEnv::workspace() const {
    return sync_->workspace();
}

// -- Pi-shaped filesystem methods ---

boost::asio::awaitable<std::expected<std::string, FileError>> AsyncLocalExecutionEnv::absolutePath(
    std::string path,
    std::stop_token stop_token) {
    co_return observe_filesystem_cancellation(stop_token, path, [&] { return sync_->absolutePath(path); });
}

boost::asio::awaitable<std::expected<std::string, FileError>> AsyncLocalExecutionEnv::joinPath(
    std::vector<std::string> parts,
    std::stop_token stop_token) {
    co_return observe_filesystem_cancellation(stop_token, std::nullopt, [&] { return sync_->joinPath(parts); });
}

boost::asio::awaitable<std::expected<std::string, FileError>> AsyncLocalExecutionEnv::readTextFile(
    std::string path,
    std::stop_token stop_token) {
    co_return observe_filesystem_cancellation(stop_token, path, [&] { return sync_->readTextFile(path); });
}

boost::asio::awaitable<std::expected<std::vector<std::string>, FileError>> AsyncLocalExecutionEnv::readTextLines(
    std::string path,
    std::optional<int> maxLines,
    std::stop_token stop_token) {
    co_return observe_filesystem_cancellation(
        stop_token, path, [&] { return sync_->readTextLines(path, maxLines); });
}

boost::asio::awaitable<std::expected<BinaryData, FileError>> AsyncLocalExecutionEnv::readBinaryFile(
    std::string path,
    std::stop_token stop_token) {
    co_return observe_filesystem_cancellation(stop_token, path, [&] { return sync_->readBinaryFile(path); });
}

boost::asio::awaitable<std::expected<void, FileError>> AsyncLocalExecutionEnv::writeFile(
    std::string path,
    WriteContent content,
    std::stop_token stop_token) {
    co_return observe_filesystem_cancellation(
        stop_token, path, [&] { return sync_->writeFile(path, content); });
}

boost::asio::awaitable<std::expected<void, FileError>> AsyncLocalExecutionEnv::appendFile(
    std::string path,
    WriteContent content,
    std::stop_token stop_token) {
    co_return observe_filesystem_cancellation(
        stop_token, path, [&] { return sync_->appendFile(path, content); });
}

boost::asio::awaitable<std::expected<FileInfo, FileError>> AsyncLocalExecutionEnv::fileInfo(
    std::string path,
    std::stop_token stop_token) {
    co_return observe_filesystem_cancellation(stop_token, path, [&] { return sync_->fileInfo(path); });
}

boost::asio::awaitable<std::expected<std::vector<FileInfo>, FileError>> AsyncLocalExecutionEnv::listDir(
    std::string path,
    std::stop_token stop_token) {
    co_return observe_filesystem_cancellation(stop_token, path, [&] { return sync_->listDir(path); });
}

boost::asio::awaitable<std::expected<std::string, FileError>> AsyncLocalExecutionEnv::canonicalPath(
    std::string path,
    std::stop_token stop_token) {
    co_return observe_filesystem_cancellation(stop_token, path, [&] { return sync_->canonicalPath(path); });
}

boost::asio::awaitable<std::expected<bool, FileError>> AsyncLocalExecutionEnv::exists(
    std::string path,
    std::stop_token stop_token) {
    co_return observe_filesystem_cancellation(stop_token, path, [&] { return sync_->exists(path); });
}

boost::asio::awaitable<std::expected<void, FileError>> AsyncLocalExecutionEnv::createDir(
    std::string path,
    bool recursive,
    std::stop_token stop_token) {
    co_return observe_filesystem_cancellation(
        stop_token, path, [&] { return sync_->createDir(path, recursive); });
}

boost::asio::awaitable<std::expected<void, FileError>> AsyncLocalExecutionEnv::remove(
    std::string path,
    bool recursive,
    std::stop_token stop_token) {
    co_return observe_filesystem_cancellation(
        stop_token, path, [&] { return sync_->remove(path, recursive); });
}

boost::asio::awaitable<std::expected<std::string, FileError>> AsyncLocalExecutionEnv::createTempDir(
    std::optional<std::string> prefix,
    std::stop_token stop_token) {
    co_return observe_filesystem_cancellation(
        stop_token, std::nullopt, [&] { return sync_->createTempDir(prefix); });
}

boost::asio::awaitable<std::expected<std::string, FileError>> AsyncLocalExecutionEnv::createTempFile(
    std::optional<std::string> prefix,
    std::optional<std::string> suffix,
    std::stop_token stop_token) {
    co_return observe_filesystem_cancellation(
        stop_token, std::nullopt, [&] { return sync_->createTempFile(prefix, suffix); });
}

// -- Pi-shaped shell method ---

boost::asio::awaitable<std::expected<ShellExecResult, ExecutionError>> AsyncLocalExecutionEnv::exec(
    std::string command,
    ExecOptions options) {
    auto request = sync_->make_exec_request(std::move(command), std::move(options));
    if (!request) {
        co_return std::unexpected(classify_execution_error(
            request.error(), ExecutionErrorOrigin::Request));
    }
    auto process = co_await sync_->process_runner()->run(std::move(*request));
    if (!process) {
        co_return std::unexpected(classify_execution_error(
            process.error(), ExecutionErrorOrigin::Process));
    }
    if (process->timed_out) {
        co_return std::unexpected(ExecutionError{ExecutionErrorCode::Timeout, "shell command timed out"});
    }
    co_return sync_->exec_result_from_process(*process);
}

} // namespace cch::harness
