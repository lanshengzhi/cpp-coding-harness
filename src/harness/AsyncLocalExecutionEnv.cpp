#include "../../include/cch/harness/LocalExecutionEnv.hpp"

#include "LocalExecutionEnv.hpp"
#include "../util/Process.hpp"

#include <utility>

namespace cch::harness {

AsyncLocalExecutionEnv::AsyncLocalExecutionEnv(
    std::filesystem::path workspace,
    bool bash_enabled,
    std::vector<std::string> secret_environment_names) {
    sync_ = std::make_shared<LocalExecutionEnv>(
        std::move(workspace),
        bash_enabled,
        std::move(secret_environment_names));
}

const std::filesystem::path& AsyncLocalExecutionEnv::workspace() const {
    return sync_->workspace();
}

bool AsyncLocalExecutionEnv::bash_enabled() const {
    return sync_->bash_enabled();
}

// -- Tool-shaped methods ---

boost::asio::awaitable<util::Expected<AsyncFileReadResult>> AsyncLocalExecutionEnv::read_file(
    std::string path,
    int offset,
    int limit) {
    co_return sync_->read_file(std::move(path), offset, limit);
}

boost::asio::awaitable<util::Expected<AsyncFileWriteResult>> AsyncLocalExecutionEnv::write_file(
    std::string path,
    std::string content,
    bool create_parents) {
    co_return sync_->write_file(std::move(path), std::move(content), create_parents);
}

boost::asio::awaitable<util::Expected<AsyncFileEditResult>> AsyncLocalExecutionEnv::edit_file(
    std::string path,
    std::string old_text,
    std::string new_text) {
    co_return sync_->edit_file(std::move(path), std::move(old_text), std::move(new_text));
}

boost::asio::awaitable<util::Expected<AsyncShellResult>> AsyncLocalExecutionEnv::run_shell(
    std::string command,
    std::chrono::milliseconds timeout) {
    auto request = sync_->make_shell_request(std::move(command), timeout);
    if (!request) {
        co_return std::unexpected(request.error());
    }
    auto process = co_await sync_->process_runner()->run(std::move(*request));
    if (!process) {
        co_return std::unexpected(process.error());
    }
    co_return sync_->shell_result_from_process(*process);
}

// -- Pi-shaped filesystem methods ---

boost::asio::awaitable<std::expected<std::string, FileError>> AsyncLocalExecutionEnv::absolutePath(
    std::string path) {
    co_return sync_->absolutePath(std::move(path));
}

boost::asio::awaitable<std::expected<std::string, FileError>> AsyncLocalExecutionEnv::joinPath(
    std::vector<std::string> parts) {
    co_return sync_->joinPath(std::move(parts));
}

boost::asio::awaitable<std::expected<std::string, FileError>> AsyncLocalExecutionEnv::readTextFile(
    std::string path) {
    co_return sync_->readTextFile(std::move(path));
}

boost::asio::awaitable<std::expected<std::vector<std::string>, FileError>> AsyncLocalExecutionEnv::readTextLines(
    std::string path,
    std::optional<int> maxLines) {
    co_return sync_->readTextLines(std::move(path), maxLines);
}

boost::asio::awaitable<std::expected<BinaryData, FileError>> AsyncLocalExecutionEnv::readBinaryFile(
    std::string path) {
    co_return sync_->readBinaryFile(std::move(path));
}

boost::asio::awaitable<std::expected<void, FileError>> AsyncLocalExecutionEnv::writeFile(
    std::string path,
    WriteContent content) {
    co_return sync_->writeFile(std::move(path), std::move(content));
}

boost::asio::awaitable<std::expected<void, FileError>> AsyncLocalExecutionEnv::appendFile(
    std::string path,
    WriteContent content) {
    co_return sync_->appendFile(std::move(path), std::move(content));
}

boost::asio::awaitable<std::expected<FileInfo, FileError>> AsyncLocalExecutionEnv::fileInfo(
    std::string path) {
    co_return sync_->fileInfo(std::move(path));
}

boost::asio::awaitable<std::expected<std::vector<FileInfo>, FileError>> AsyncLocalExecutionEnv::listDir(
    std::string path) {
    co_return sync_->listDir(std::move(path));
}

boost::asio::awaitable<std::expected<std::string, FileError>> AsyncLocalExecutionEnv::canonicalPath(
    std::string path) {
    co_return sync_->canonicalPath(std::move(path));
}

boost::asio::awaitable<std::expected<bool, FileError>> AsyncLocalExecutionEnv::exists(
    std::string path) {
    co_return sync_->exists(std::move(path));
}

boost::asio::awaitable<std::expected<void, FileError>> AsyncLocalExecutionEnv::createDir(
    std::string path,
    bool recursive) {
    co_return sync_->createDir(std::move(path), recursive);
}

boost::asio::awaitable<std::expected<void, FileError>> AsyncLocalExecutionEnv::remove(
    std::string path,
    bool recursive) {
    co_return sync_->remove(std::move(path), recursive);
}

boost::asio::awaitable<std::expected<std::string, FileError>> AsyncLocalExecutionEnv::createTempDir(
    std::optional<std::string> prefix) {
    co_return sync_->createTempDir(std::move(prefix));
}

boost::asio::awaitable<std::expected<std::string, FileError>> AsyncLocalExecutionEnv::createTempFile(
    std::optional<std::string> prefix,
    std::optional<std::string> suffix) {
    co_return sync_->createTempFile(std::move(prefix), std::move(suffix));
}

// -- Pi-shaped shell method ---

boost::asio::awaitable<std::expected<ShellExecResult, ExecutionError>> AsyncLocalExecutionEnv::exec(
    std::string command,
    ExecOptions options) {
    auto request = sync_->make_exec_request(std::move(command), std::move(options));
    if (!request) {
        if (request.error().detail.find("disabled") != std::string::npos) {
            co_return std::unexpected(ExecutionError{ExecutionErrorCode::ShellUnavailable, request.error().detail});
        }
        co_return std::unexpected(ExecutionError{ExecutionErrorCode::SpawnError, request.error().detail});
    }
    auto process = co_await sync_->process_runner()->run(std::move(*request));
    if (!process) {
        const auto& err = process.error();
        auto code = ExecutionErrorCode::Unknown;
        if (err.code == util::ErrorCode::Timeout) {
            code = ExecutionErrorCode::Timeout;
        } else if (err.detail.find("callback") != std::string::npos) {
            code = ExecutionErrorCode::CallbackError;
        } else {
            code = ExecutionErrorCode::SpawnError;
        }
        co_return std::unexpected(ExecutionError{code, err.detail});
    }
    if (process->timed_out) {
        co_return std::unexpected(ExecutionError{ExecutionErrorCode::Timeout, "shell command timed out"});
    }
    co_return sync_->exec_result_from_process(*process);
}

} // namespace cch::harness
