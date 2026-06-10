#include <cch/harness/LocalExecutionEnv.hpp>

#include "LocalExecutionEnv.hpp"
#include "../util/Process.hpp"

#include <utility>

namespace cch::harness {
namespace {

template <typename T>
[[nodiscard]] util::Expected<T> legacy_failure(std::string message, util::ErrorCode code = util::ErrorCode::Tool) {
    return std::unexpected(util::make_error(code, std::move(message), message));
}

} // namespace

AsyncLocalExecutionEnv::AsyncLocalExecutionEnv(
    std::filesystem::path workspace,
    bool bash_enabled,
    std::vector<std::string> secret_environment_names,
    std::shared_ptr<util::ProcessRunner> runner) {
    if (!runner) {
        runner = std::make_shared<util::DefaultProcessRunner>();
    }
    sync_ = std::make_shared<LocalExecutionEnv>(
        std::move(workspace),
        bash_enabled,
        std::move(secret_environment_names),
        std::move(runner));
}

const std::filesystem::path& AsyncLocalExecutionEnv::workspace() const {
    return sync_->workspace();
}

bool AsyncLocalExecutionEnv::bash_enabled() const {
    return sync_->bash_enabled();
}

boost::asio::awaitable<util::Expected<AsyncFileReadResult>> AsyncLocalExecutionEnv::read_file(
    std::string path,
    int offset,
    int limit) {
    auto result = sync_->read_file(path, offset, limit);
    if (!result) {
        co_return legacy_failure<AsyncFileReadResult>(result.error(), util::ErrorCode::Workspace);
    }
    co_return AsyncFileReadResult{result.value().content, result.value().truncated};
}

boost::asio::awaitable<util::Expected<AsyncFileWriteResult>> AsyncLocalExecutionEnv::write_file(
    std::string path,
    std::string content,
    bool create_parents) {
    auto result = sync_->write_file(path, content, create_parents);
    if (!result) {
        co_return legacy_failure<AsyncFileWriteResult>(result.error(), util::ErrorCode::Workspace);
    }
    co_return AsyncFileWriteResult{result.value().bytes_written};
}

boost::asio::awaitable<util::Expected<AsyncFileEditResult>> AsyncLocalExecutionEnv::edit_file(
    std::string path,
    std::string old_text,
    std::string new_text) {
    auto result = sync_->edit_file(path, old_text, new_text);
    if (!result) {
        co_return legacy_failure<AsyncFileEditResult>(result.error(), util::ErrorCode::Workspace);
    }
    co_return AsyncFileEditResult{result.value().old_preview, result.value().new_preview};
}

boost::asio::awaitable<util::Expected<AsyncShellResult>> AsyncLocalExecutionEnv::run_shell(
    std::string command,
    std::chrono::milliseconds timeout) {
    auto result = sync_->run_shell(command, timeout);
    if (!result) {
        co_return legacy_failure<AsyncShellResult>(result.error(), util::ErrorCode::Process);
    }
    co_return AsyncShellResult{result.value().exit_code, result.value().output, result.value().timed_out};
}

} // namespace cch::harness
