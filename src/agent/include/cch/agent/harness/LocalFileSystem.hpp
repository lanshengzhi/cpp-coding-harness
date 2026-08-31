#pragma once

#include <cch/agent/harness/FileSystem.hpp>

#include <filesystem>
#include <memory>

namespace cch::harness {

class RuntimeTarget;

/// Local filesystem Adapter for the complete asynchronous filesystem
/// capability. All containment-sensitive work delegates to the private
/// WorkspaceFileSystem implementation through the adapter's private state.
class AsyncLocalFileSystem final : public AsyncFileSystem {
public:
    AsyncLocalFileSystem(std::shared_ptr<RuntimeTarget> runtime_target, std::filesystem::path workspace);
    AsyncLocalFileSystem(AsyncLocalFileSystem&&) noexcept;
    AsyncLocalFileSystem& operator=(AsyncLocalFileSystem&&) noexcept;
    ~AsyncLocalFileSystem() override;
    AsyncLocalFileSystem(const AsyncLocalFileSystem&) = delete;
    AsyncLocalFileSystem& operator=(const AsyncLocalFileSystem&) = delete;

    [[nodiscard]] const std::filesystem::path& workspace() const override;

    [[nodiscard]] support::AsyncResult<std::string, FileError> absolutePath(
            std::string path, std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<std::string, FileError> joinPath(
            std::vector<std::string> parts, std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<std::string, FileError> readTextFile(
            std::string path, std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<std::vector<std::string>, FileError> readTextLines(
            std::string path, std::optional<int> maxLines, std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<BinaryData, FileError> readBinaryFile(
            std::string path, std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<void, FileError> writeFile(
            std::string path, WriteContent content, std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<void, FileError> appendFile(
            std::string path, WriteContent content, std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<FileInfo, FileError> fileInfo(
            std::string path, std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<std::vector<FileInfo>, FileError> listDir(
            std::string path, std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<std::string, FileError> canonicalPath(
            std::string path, std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<bool, FileError> exists(std::string path, std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<void, FileError> createDir(
            std::string path, bool recursive, std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<void, FileError> remove(
            std::string path, bool recursive, std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<std::string, FileError> createTempDir(
            std::optional<std::string> prefix, std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<std::string, FileError> createTempFile(
            std::optional<std::string> prefix, std::optional<std::string> suffix, std::stop_token stop_token) override;
    [[nodiscard]] support::AsyncResult<void, FileError> cleanup() override;

private:
    struct Impl;

    std::shared_ptr<Impl> impl_;
};

} // namespace cch::harness
