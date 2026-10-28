#pragma once

#include <cch/durable/Workflow.hpp>
#include <cch/support/Error.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cch::durable {

struct FileInfo {
    std::string name;
    std::string path;
    std::string kind;
    std::uintmax_t size{};
};

struct ShellResult {
    int exit_code{};
    std::string output;
    std::string standard_output;
    std::string standard_error;
};

using ShellCommand = std::variant<std::string, std::vector<std::string>>;
using OutputSink = std::function<void(std::string_view, bool)>;

class FileWatch {
public:
    virtual ~FileWatch() = default;
    virtual void close() = 0;
};

/// Physical operations used by durable tools. Implementations and their callers
/// may run concurrently; the supplied environment must outlive the extension.
class Environment {
public:
    virtual ~Environment() = default;
    virtual support::Expected<std::string> read(std::string_view path, std::stop_token stop) = 0;
    virtual support::ExpectedVoid write(std::string_view path, std::string_view content, std::stop_token stop) = 0;
    virtual support::ExpectedVoid append(std::string_view path, std::string_view content, std::stop_token stop) = 0;
    virtual support::Expected<FileInfo> stat(std::string_view path, std::stop_token stop) = 0;
    virtual support::Expected<std::vector<FileInfo>> list(std::string_view path, std::stop_token stop) = 0;
    virtual support::Expected<ShellResult>
    exec(const ShellCommand& command, std::chrono::milliseconds timeout, OutputSink output, std::stop_token stop) = 0;
    /// Recursive watching is not implemented by the local adapter and is rejected.
    virtual support::Expected<std::unique_ptr<FileWatch>> watch(
            std::string_view path, std::function<void()> on_change, bool recursive = false) = 0;
};

/// Construct the durable-owned Linux environment rooted at cwd.
[[nodiscard]] std::shared_ptr<Environment> local_environment(std::string cwd);

/// Construct the explicit coding-tools extension; no automatic installation.
[[nodiscard]] Extension coding_tools(std::shared_ptr<Environment> environment);

} // namespace cch::durable
