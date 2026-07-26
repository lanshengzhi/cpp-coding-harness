#pragma once

#include <cch/harness/ExecutionEnv.hpp>

#include <memory>
#include <vector>

namespace cch::harness {

class AsyncLocalExecutionEnv final : public AsyncExecutionEnv {
public:
    AsyncLocalExecutionEnv(
        std::filesystem::path workspace,
        bool bash_enabled = false,
        std::vector<std::string> secret_environment_names = {});
    AsyncLocalExecutionEnv(AsyncLocalExecutionEnv&&) noexcept;
    AsyncLocalExecutionEnv& operator=(AsyncLocalExecutionEnv&&) noexcept;
    ~AsyncLocalExecutionEnv() override;
    AsyncLocalExecutionEnv(const AsyncLocalExecutionEnv&) = delete;
    AsyncLocalExecutionEnv& operator=(const AsyncLocalExecutionEnv&) = delete;

    [[nodiscard]] const std::filesystem::path& workspace() const override;

    // -- Pi-shaped filesystem overrides ---

    [[nodiscard]] boost::asio::awaitable<std::expected<std::string, FileError>> absolutePath(
        std::string path,
        std::stop_token stop_token) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<std::string, FileError>> joinPath(
        std::vector<std::string> parts,
        std::stop_token stop_token) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<std::string, FileError>> readTextFile(
        std::string path,
        std::stop_token stop_token) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<std::vector<std::string>, FileError>> readTextLines(
        std::string path,
        std::optional<int> maxLines,
        std::stop_token stop_token) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<BinaryData, FileError>> readBinaryFile(
        std::string path,
        std::stop_token stop_token) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<void, FileError>> writeFile(
        std::string path,
        WriteContent content,
        std::stop_token stop_token) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<void, FileError>> appendFile(
        std::string path,
        WriteContent content,
        std::stop_token stop_token) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<FileInfo, FileError>> fileInfo(
        std::string path,
        std::stop_token stop_token) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<std::vector<FileInfo>, FileError>> listDir(
        std::string path,
        std::stop_token stop_token) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<std::string, FileError>> canonicalPath(
        std::string path,
        std::stop_token stop_token) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<bool, FileError>> exists(
        std::string path,
        std::stop_token stop_token) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<void, FileError>> createDir(
        std::string path,
        bool recursive,
        std::stop_token stop_token) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<void, FileError>> remove(
        std::string path,
        bool recursive,
        std::stop_token stop_token) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<std::string, FileError>> createTempDir(
        std::optional<std::string> prefix,
        std::stop_token stop_token) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<std::string, FileError>> createTempFile(
        std::optional<std::string> prefix,
        std::optional<std::string> suffix,
        std::stop_token stop_token) override;

    // -- Pi-shaped shell override ---

    [[nodiscard]] boost::asio::awaitable<std::expected<ShellExecResult, ExecutionError>> exec(
        std::string command,
        ExecOptions options = {}) override;

private:
    std::unique_ptr<class SyncLocalExecutionEnv> sync_;
};

} // namespace cch::harness
