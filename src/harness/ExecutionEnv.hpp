#pragma once

#include "../util/Result.hpp"

#include <chrono>
#include <filesystem>
#include <string>

namespace cch::harness {

struct FileReadResult {
    std::string content;
    bool truncated{false};
};

struct FileWriteResult {
    std::size_t bytes_written{0};
};

struct FileEditResult {
    std::string old_preview;
    std::string new_preview;
};

struct ShellResult {
    int exit_code{-1};
    std::string output;
    bool timed_out{false};
};

class ExecutionEnv {
public:
    virtual ~ExecutionEnv() = default;

    [[nodiscard]] virtual const std::filesystem::path& workspace() const = 0;
    [[nodiscard]] virtual bool bash_enabled() const = 0;

    [[nodiscard]] virtual util::Result<FileReadResult> read_file(const std::string& path, int offset, int limit) = 0;
    [[nodiscard]] virtual util::Result<FileWriteResult> write_file(const std::string& path, const std::string& content, bool create_parents) = 0;
    [[nodiscard]] virtual util::Result<FileEditResult> edit_file(const std::string& path, const std::string& old_text, const std::string& new_text) = 0;
    [[nodiscard]] virtual util::Result<ShellResult> run_shell(const std::string& command, std::chrono::milliseconds timeout) = 0;
};

} // namespace cch::harness
