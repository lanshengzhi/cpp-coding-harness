#pragma once

#include "ExecutionEnv.hpp"

#include <memory>
#include <vector>

namespace cch::harness {

class AsyncLocalExecutionEnv final : public AsyncExecutionEnv {
public:
    AsyncLocalExecutionEnv(
        std::filesystem::path workspace,
        bool bash_enabled = false,
        std::vector<std::string> secret_environment_names = {});

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

    // -- Pi-shaped filesystem overrides ---

    [[nodiscard]] boost::asio::awaitable<std::expected<std::string, FileError>> absolutePath(
        std::string path) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<std::string, FileError>> joinPath(
        std::vector<std::string> parts) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<std::string, FileError>> readTextFile(
        std::string path) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<std::vector<std::string>, FileError>> readTextLines(
        std::string path,
        std::optional<int> maxLines = std::nullopt) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<BinaryData, FileError>> readBinaryFile(
        std::string path) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<void, FileError>> writeFile(
        std::string path,
        WriteContent content) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<void, FileError>> appendFile(
        std::string path,
        WriteContent content) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<FileInfo, FileError>> fileInfo(
        std::string path) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<std::vector<FileInfo>, FileError>> listDir(
        std::string path) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<std::string, FileError>> canonicalPath(
        std::string path) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<bool, FileError>> exists(
        std::string path) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<void, FileError>> createDir(
        std::string path,
        bool recursive = true) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<void, FileError>> remove(
        std::string path,
        bool recursive = false) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<std::string, FileError>> createTempDir(
        std::optional<std::string> prefix = std::nullopt) override;
    [[nodiscard]] boost::asio::awaitable<std::expected<std::string, FileError>> createTempFile(
        std::optional<std::string> prefix = std::nullopt,
        std::optional<std::string> suffix = std::nullopt) override;

    // -- Pi-shaped shell override ---

    [[nodiscard]] boost::asio::awaitable<std::expected<ShellExecResult, ExecutionError>> exec(
        std::string command,
        ExecOptions options = {}) override;

private:
    std::shared_ptr<class SyncLocalExecutionEnv> sync_;
};

} // namespace cch::harness
