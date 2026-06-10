#pragma once

#include "ExecutionEnv.hpp"

#include <memory>
#include <vector>

namespace cch::util {
class ProcessRunner;
class DefaultProcessRunner;
} // namespace cch::util

namespace cch::harness {

class AsyncLocalExecutionEnv final : public AsyncExecutionEnv {
public:
    AsyncLocalExecutionEnv(
        std::filesystem::path workspace,
        bool bash_enabled = false,
        std::vector<std::string> secret_environment_names = {},
        std::shared_ptr<util::ProcessRunner> runner = nullptr);

    [[nodiscard]] const std::filesystem::path& workspace() const override;
    [[nodiscard]] bool bash_enabled() const override;

    [[nodiscard]] boost::asio::awaitable<util::Expected<AsyncFileReadResult>> read_file(
        std::string path,
        int offset,
        int limit) override;
    [[nodiscard]] boost::asio::awaitable<util::Expected<AsyncFileWriteResult>> write_file(
        std::string path,
        std::string content,
        bool create_parents) override;
    [[nodiscard]] boost::asio::awaitable<util::Expected<AsyncFileEditResult>> edit_file(
        std::string path,
        std::string old_text,
        std::string new_text) override;
    [[nodiscard]] boost::asio::awaitable<util::Expected<AsyncShellResult>> run_shell(
        std::string command,
        std::chrono::milliseconds timeout) override;

private:
    std::shared_ptr<class LocalExecutionEnv> sync_;
};

} // namespace cch::harness
