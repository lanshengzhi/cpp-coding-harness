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
    co_return sync_->run_shell(std::move(command), timeout);
}

} // namespace cch::harness
