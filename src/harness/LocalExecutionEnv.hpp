#pragma once

#include "../../include/cch/harness/ExecutionEnv.hpp"

#include "../util/Process.hpp"

#include <memory>
#include <vector>

namespace cch::harness {

class LocalExecutionEnv final {
public:
    LocalExecutionEnv(
        std::filesystem::path workspace,
        bool bash_enabled = false,
        std::vector<std::string> secret_environment_names = {},
        std::shared_ptr<util::ProcessRunner> runner = std::make_shared<util::DefaultProcessRunner>());

    [[nodiscard]] const std::filesystem::path& workspace() const { return workspace_; }
    [[nodiscard]] bool bash_enabled() const { return bash_enabled_; }

    [[nodiscard]] util::Expected<AsyncFileReadResult> read_file(std::string path, int offset, int limit);
    [[nodiscard]] util::Expected<AsyncFileWriteResult> write_file(std::string path, std::string content, bool create_parents);
    [[nodiscard]] util::Expected<AsyncFileEditResult> edit_file(std::string path, std::string old_text, std::string new_text);
    [[nodiscard]] util::Expected<AsyncShellResult> run_shell(std::string command, std::chrono::milliseconds timeout);

    [[nodiscard]] util::Expected<util::ProcessRequest> make_shell_request(
        std::string command,
        std::chrono::milliseconds timeout) const;
    [[nodiscard]] AsyncShellResult shell_result_from_process(const util::ProcessResult& process) const;
    [[nodiscard]] std::shared_ptr<util::ProcessRunner> process_runner() const { return runner_; }

private:
    std::filesystem::path workspace_;
    bool bash_enabled_{false};
    std::vector<std::string> secret_environment_names_;
    std::shared_ptr<util::ProcessRunner> runner_;
};

} // namespace cch::harness
