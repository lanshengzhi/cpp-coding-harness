#pragma once

#include <cch/agent/harness/ExecutionEnv.hpp>

#include <memory>
#include <vector>

namespace cch::harness {

class RuntimeTarget;

/// Local Execution Environment bound to a Runtime root target. The target is
/// shared deliberately with pending operations, so destroying this facade
/// cannot strand admitted work or leave it using destroyed filesystem state.
class AsyncLocalExecutionEnv final : public AsyncExecutionEnv {
public:
    AsyncLocalExecutionEnv(
        std::shared_ptr<RuntimeTarget> runtime_target,
        std::filesystem::path workspace,
        bool bash_enabled = false,
        std::vector<std::string> secret_environment_names = {},
        ShellConfig shell_config = {});
    AsyncLocalExecutionEnv(AsyncLocalExecutionEnv&&) noexcept;
    AsyncLocalExecutionEnv& operator=(AsyncLocalExecutionEnv&&) noexcept;
    ~AsyncLocalExecutionEnv() override;
    AsyncLocalExecutionEnv(const AsyncLocalExecutionEnv&) = delete;
    AsyncLocalExecutionEnv& operator=(const AsyncLocalExecutionEnv&) = delete;

    [[nodiscard]] const std::filesystem::path& workspace() const override;

    // -- Pi-shaped filesystem overrides ---

    [[nodiscard]] support::AsyncResult<std::string, FileError> absolutePath(
        std::string path,
        std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<std::string, FileError> joinPath(
        std::vector<std::string> parts,
        std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<std::string, FileError> readTextFile(
        std::string path,
        std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<std::vector<std::string>, FileError> readTextLines(
        std::string path,
        std::optional<int> maxLines,
        std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<BinaryData, FileError> readBinaryFile(
        std::string path,
        std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<void, FileError> writeFile(
        std::string path,
        WriteContent content,
        std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<void, FileError> appendFile(
        std::string path,
        WriteContent content,
        std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<FileInfo, FileError> fileInfo(
        std::string path,
        std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<std::vector<FileInfo>, FileError> listDir(
        std::string path,
        std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<std::string, FileError> canonicalPath(
        std::string path,
        std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<bool, FileError> exists(
        std::string path,
        std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<void, FileError> createDir(
        std::string path,
        bool recursive,
        std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<void, FileError> remove(
        std::string path,
        bool recursive,
        std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<std::string, FileError> createTempDir(
        std::optional<std::string> prefix,
        std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<std::string, FileError> createTempFile(
        std::optional<std::string> prefix,
        std::optional<std::string> suffix,
        std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<void, FileError> cleanup() override;

    // -- Pi-shaped shell override ---

    [[nodiscard]] support::AsyncResult<ShellExecResult, ExecutionError> exec(
        std::string command,
        ExecOptions options = {}) override;

private:
    struct Impl;

    std::shared_ptr<Impl> impl_;
};

} // namespace cch::harness
