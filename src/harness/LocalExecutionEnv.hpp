#pragma once

#include "../../include/cch/harness/ExecutionEnv.hpp"

#include "../util/Process.hpp"
#include "WorkspaceFileSystem.hpp"

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

    // -- Existing tool-shaped methods ---

    [[nodiscard]] util::Expected<AsyncFileReadResult> read_file(std::string path, int offset, int limit);
    [[nodiscard]] util::Expected<AsyncFileWriteResult> write_file(std::string path, std::string content, bool create_parents);
    [[nodiscard]] util::Expected<AsyncFileEditResult> edit_file(std::string path, std::string old_text, std::string new_text);
    [[nodiscard]] util::Expected<AsyncShellResult> run_shell(std::string command, std::chrono::milliseconds timeout);

    [[nodiscard]] util::Expected<util::ProcessRequest> make_shell_request(
        std::string command,
        std::chrono::milliseconds timeout) const;
    [[nodiscard]] AsyncShellResult shell_result_from_process(const util::ProcessResult& process) const;
    [[nodiscard]] std::shared_ptr<util::ProcessRunner> process_runner() const { return runner_; }

    // -- Pi-shaped filesystem methods ---

    [[nodiscard]] std::expected<std::string, FileError> absolutePath(const std::string& path) const;
    [[nodiscard]] std::expected<std::string, FileError> joinPath(const std::vector<std::string>& parts) const;
    [[nodiscard]] std::expected<std::string, FileError> readTextFile(const std::string& path) const;
    [[nodiscard]] std::expected<std::vector<std::string>, FileError> readTextLines(
        const std::string& path,
        std::optional<int> maxLines = std::nullopt) const;
    [[nodiscard]] std::expected<BinaryData, FileError> readBinaryFile(const std::string& path) const;
    [[nodiscard]] std::expected<void, FileError> writeFile(const std::string& path, const WriteContent& content) const;
    [[nodiscard]] std::expected<void, FileError> appendFile(const std::string& path, const WriteContent& content) const;
    [[nodiscard]] std::expected<FileInfo, FileError> fileInfo(const std::string& path) const;
    [[nodiscard]] std::expected<std::vector<FileInfo>, FileError> listDir(const std::string& path) const;
    [[nodiscard]] std::expected<std::string, FileError> canonicalPath(const std::string& path) const;
    [[nodiscard]] std::expected<bool, FileError> exists(const std::string& path) const;
    [[nodiscard]] std::expected<void, FileError> createDir(const std::string& path, bool recursive = true) const;
    [[nodiscard]] std::expected<void, FileError> remove(const std::string& path, bool recursive = false) const;
    [[nodiscard]] std::expected<std::string, FileError> createTempDir(
        std::optional<std::string> prefix = std::nullopt) const;
    [[nodiscard]] std::expected<std::string, FileError> createTempFile(
        std::optional<std::string> prefix = std::nullopt,
        std::optional<std::string> suffix = std::nullopt) const;

    [[nodiscard]] const WorkspaceFileSystem& fs() const { return fs_; }

    // -- Pi-shaped shell methods ---

    [[nodiscard]] util::Expected<util::ProcessRequest> make_exec_request(
        std::string command,
        ExecOptions options) const;
    [[nodiscard]] ShellExecResult exec_result_from_process(const util::ProcessResult& process) const;
    [[nodiscard]] std::expected<ShellExecResult, ExecutionError> exec(
        std::string command,
        ExecOptions options = {}) const;

private:
    std::filesystem::path workspace_;
    bool bash_enabled_{false};
    std::vector<std::string> secret_environment_names_;
    std::shared_ptr<util::ProcessRunner> runner_;
    WorkspaceFileSystem fs_;
};

} // namespace cch::harness
