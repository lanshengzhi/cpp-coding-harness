#pragma once

#include <cch/harness/ExecutionEnv.hpp>
#include "coding_agent/runtime/AsyncUserShell.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace cch::coding_agent::runtime {

/// Production local User Shell: the Native TUI's independent direct-user
/// execution capability. Every invocation resolves the effective Shell lazily
/// (custom path, /bin/bash, PATH bash, then sh, always with -c), launches in
/// the canonical Session workspace with the project's filtered environment,
/// merges stdout/stderr into one emission-ordered stream, runs without a
/// timeout, and cancels through process-tree termination. Construction is
/// infallible; a stale configured shell path fails only the attempted
/// execution.
class LocalUserShell final : public AsyncUserShell {
public:
    LocalUserShell(
        std::filesystem::path workspace,
        std::vector<std::string> secret_environment_names,
        harness::ShellConfig shell_config = {});

    [[nodiscard]] support::AsyncResult<UserShellResult> execute(
        std::string command,
        UserShellUpdateSink update_sink,
        std::stop_token stop_token) override;

private:
    std::filesystem::path workspace_;
    std::vector<std::string> secret_environment_names_;
    harness::ShellConfig shell_config_;
};

} // namespace cch::coding_agent::runtime
