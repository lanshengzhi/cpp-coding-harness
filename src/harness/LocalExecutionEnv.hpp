#pragma once

#include "ExecutionEnv.hpp"
#include "../util/Process.hpp"

#include <memory>
#include <vector>

namespace cch::harness {

class LocalExecutionEnv final : public ExecutionEnv {
public:
    LocalExecutionEnv(
        std::filesystem::path workspace,
        bool bash_enabled = false,
        std::vector<std::string> secret_environment_names = {},
        std::shared_ptr<util::ProcessRunner> runner = std::make_shared<util::DefaultProcessRunner>());

    [[nodiscard]] const std::filesystem::path& workspace() const override { return workspace_; }
    [[nodiscard]] bool bash_enabled() const override { return bash_enabled_; }

    [[nodiscard]] util::Result<FileReadResult> read_file(const std::string& path, int offset, int limit) override;
    [[nodiscard]] util::Result<FileWriteResult> write_file(const std::string& path, const std::string& content, bool create_parents) override;
    [[nodiscard]] util::Result<FileEditResult> edit_file(const std::string& path, const std::string& old_text, const std::string& new_text) override;
    [[nodiscard]] util::Result<ShellResult> run_shell(const std::string& command, std::chrono::milliseconds timeout) override;

private:
    std::filesystem::path workspace_;
    bool bash_enabled_{false};
    std::vector<std::string> secret_environment_names_;
    std::shared_ptr<util::ProcessRunner> runner_;
};

} // namespace cch::harness
