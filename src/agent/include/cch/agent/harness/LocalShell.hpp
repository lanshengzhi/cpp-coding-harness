#pragma once

#include <cch/agent/harness/Shell.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace cch::harness {

class RuntimeTarget;
class AsyncProcessRunner;

/// Local Shell Adapter for the complete asynchronous Shell capability. Shell
/// and process behavior concentrates here: working-directory overrides
/// validate through the private WorkspaceFileSystem containment
/// implementation, the base environment is sanitized, and processes launch
/// through the private process runner. The Runtime target is shared
/// deliberately with pending operations, so destroying this adapter cannot
/// strand admitted work.
class AsyncLocalShell final : public AsyncShell {
public:
    AsyncLocalShell(std::shared_ptr<RuntimeTarget> runtime_target,
            std::filesystem::path workspace,
            bool bash_enabled = false,
            std::vector<std::string> secret_environment_names = {},
            ShellConfig shell_config = {});
    /// Process-runner injection seam used by the Local Shell adapter tests;
    /// production assembly uses the default constructor above.
    AsyncLocalShell(std::shared_ptr<RuntimeTarget> runtime_target,
            std::filesystem::path workspace,
            bool bash_enabled,
            std::vector<std::string> secret_environment_names,
            ShellConfig shell_config,
            std::shared_ptr<AsyncProcessRunner> runner);
    AsyncLocalShell(AsyncLocalShell&&) noexcept;
    AsyncLocalShell& operator=(AsyncLocalShell&&) noexcept;
    ~AsyncLocalShell() override;
    AsyncLocalShell(const AsyncLocalShell&) = delete;
    AsyncLocalShell& operator=(const AsyncLocalShell&) = delete;

    [[nodiscard]] support::AsyncResult<ShellExecResult, ExecutionError> exec(
            std::string command, ExecOptions options = {}) override;

private:
    struct Impl;

    std::shared_ptr<Impl> impl_;
};

} // namespace cch::harness
