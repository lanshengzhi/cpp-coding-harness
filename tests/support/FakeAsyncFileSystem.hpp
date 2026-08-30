#pragma once

#include <cch/agent/harness/FileSystem.hpp>

#include <filesystem>
#include <optional>
#include <utility>

namespace cch::tests {

/// Complete deterministic filesystem fake for contract tests. A configured
/// next error is returned by every ordinary operation; a pre-requested stop
/// token wins with FileErrorCode::Aborted. It deliberately has no physical
/// filesystem or scheduler dependency.
class FakeAsyncFileSystem final : public harness::AsyncFileSystem {
public:
    explicit FakeAsyncFileSystem(std::filesystem::path workspace)
        : workspace_(std::move(workspace)) {}

    const std::filesystem::path& workspace() const override { return workspace_; }

    support::AsyncResult<std::string, harness::FileError> absolutePath(
        std::string path,
        std::stop_token stop_token) override {
        if (auto error = failure(stop_token)) {
            return failed<std::string>(std::move(*error));
        }
        return ready(std::move(path));
    }

    support::AsyncResult<std::string, harness::FileError> joinPath(
        std::vector<std::string> parts,
        std::stop_token stop_token) override {
        if (auto error = failure(stop_token)) {
            return failed<std::string>(std::move(*error));
        }
        std::filesystem::path result = workspace_;
        for (auto& part : parts) {
            result /= std::move(part);
        }
        return ready(result.string());
    }

    support::AsyncResult<std::string, harness::FileError> readTextFile(
        std::string path,
        std::stop_token stop_token) override {
        if (auto error = failure(stop_token)) {
            return failed<std::string>(std::move(*error));
        }
        return ready(std::move(path));
    }

    support::AsyncResult<std::vector<std::string>, harness::FileError> readTextLines(
        std::string,
        std::optional<int>,
        std::stop_token stop_token) override {
        if (auto error = failure(stop_token)) {
            return failed<std::vector<std::string>>(std::move(*error));
        }
        return ready(std::vector<std::string>{});
    }

    support::AsyncResult<harness::BinaryData, harness::FileError> readBinaryFile(
        std::string,
        std::stop_token stop_token) override {
        if (auto error = failure(stop_token)) {
            return failed<harness::BinaryData>(std::move(*error));
        }
        return ready(harness::BinaryData{});
    }

    support::AsyncResult<void, harness::FileError> writeFile(
        std::string,
        harness::WriteContent,
        std::stop_token stop_token) override {
        return void_result(stop_token);
    }

    support::AsyncResult<void, harness::FileError> appendFile(
        std::string,
        harness::WriteContent,
        std::stop_token stop_token) override {
        return void_result(stop_token);
    }

    support::AsyncResult<harness::FileInfo, harness::FileError> fileInfo(
        std::string path,
        std::stop_token stop_token) override {
        if (auto error = failure(stop_token)) {
            return failed<harness::FileInfo>(std::move(*error));
        }
        return ready(harness::FileInfo{
            .name = std::move(path),
            .path = workspace_.string(),
            .kind = harness::FileKind::File,
        });
    }

    support::AsyncResult<std::vector<harness::FileInfo>, harness::FileError> listDir(
        std::string,
        std::stop_token stop_token) override {
        if (auto error = failure(stop_token)) {
            return failed<std::vector<harness::FileInfo>>(std::move(*error));
        }
        return ready(std::vector<harness::FileInfo>{});
    }

    support::AsyncResult<std::string, harness::FileError> canonicalPath(
        std::string path,
        std::stop_token stop_token) override {
        if (auto error = failure(stop_token)) {
            return failed<std::string>(std::move(*error));
        }
        return ready(std::move(path));
    }

    support::AsyncResult<bool, harness::FileError> exists(
        std::string,
        std::stop_token stop_token) override {
        if (auto error = failure(stop_token)) {
            return failed<bool>(std::move(*error));
        }
        return ready(true);
    }

    support::AsyncResult<void, harness::FileError> createDir(
        std::string,
        bool,
        std::stop_token stop_token) override {
        return void_result(stop_token);
    }

    support::AsyncResult<void, harness::FileError> remove(
        std::string,
        bool,
        std::stop_token stop_token) override {
        return void_result(stop_token);
    }

    support::AsyncResult<std::string, harness::FileError> createTempDir(
        std::optional<std::string>,
        std::stop_token stop_token) override {
        if (auto error = failure(stop_token)) {
            return failed<std::string>(std::move(*error));
        }
        return ready(workspace_.string() + "/fake-temp-dir");
    }

    support::AsyncResult<std::string, harness::FileError> createTempFile(
        std::optional<std::string>,
        std::optional<std::string>,
        std::stop_token stop_token) override {
        if (auto error = failure(stop_token)) {
            return failed<std::string>(std::move(*error));
        }
        return ready(workspace_.string() + "/fake-temp-file");
    }

    support::AsyncResult<void, harness::FileError> cleanup() override {
        if (cleanup_error) {
            return failed<void>(*cleanup_error);
        }
        return ready();
    }

    std::optional<harness::FileError> next_error;
    std::optional<harness::FileError> cleanup_error;

private:
    template <typename T>
    [[nodiscard]] static support::AsyncResult<T, harness::FileError> ready(T value) {
        return support::AsyncResult<T, harness::FileError>{
            std::expected<T, harness::FileError>{std::move(value)}};
    }

    [[nodiscard]] static support::AsyncResult<void, harness::FileError> ready() {
        return support::AsyncResult<void, harness::FileError>{
            std::expected<void, harness::FileError>{}};
    }

    template <typename T>
    [[nodiscard]] static support::AsyncResult<T, harness::FileError> failed(
        harness::FileError error) {
        return support::AsyncResult<T, harness::FileError>{
            std::unexpected(std::move(error))};
    }

    [[nodiscard]] support::AsyncResult<void, harness::FileError> void_result(
        std::stop_token stop_token) {
        if (auto error = failure(stop_token)) {
            return failed<void>(std::move(*error));
        }
        return ready();
    }

    [[nodiscard]] std::optional<harness::FileError> failure(
        std::stop_token stop_token) const {
        if (stop_token.stop_requested()) {
            return harness::FileError{
                .code = harness::FileErrorCode::Aborted,
                .message = "Operation aborted",
                .path = std::nullopt,
            };
        }
        if (next_error) {
            return next_error;
        }
        return std::nullopt;
    }

    std::filesystem::path workspace_;
};

} // namespace cch::tests
