#pragma once

#include <cch/durable/Images.hpp>
#include <cch/durable/Workflow.hpp>
#include <cch/support/Error.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
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

/// One page of one directory's entries, continuing where the previous page stopped.
struct ListingPage {
    std::vector<FileInfo> entries;
    /// The end of the directory was reached; a later page holds no entry.
    bool done{};
};

/// Pages one directory, holding it open so every page continues the same scan.
class DirectoryReader {
public:
    virtual ~DirectoryReader() = default;
    /// Up to `max_entries` entries in the order the file system returns them.
    /// `done` marks the end and may come with the last entries or with an empty
    /// page. An entry that disappears before its metadata is read is skipped.
    virtual support::Expected<ListingPage> next(std::size_t max_entries, std::stop_token stop) = 0;
};

struct ShellOutputWindow {
    std::size_t max_bytes{50000};
    std::size_t max_lines{2000};
};

struct ShellOutputSkip {
    std::size_t bytes{};
    std::size_t newlines{};
    bool ends_with_newline{};
};

struct ShellResult {
    int exit_code{};
    std::string output;
    std::string standard_output;
    std::string standard_error;
    std::optional<ShellOutputSkip> skipped;
    std::optional<ShellOutputSkip> standard_output_skipped;
    std::optional<ShellOutputSkip> standard_error_skipped;
};

using ShellCommand = std::variant<std::string, std::vector<std::string>>;
using OutputSink = std::move_only_function<void(std::string_view, bool)>;
using FileChangeSink = std::move_only_function<void()>;

struct WatchOptions {
    bool recursive{};
    bool exclude_hidden{};
    std::vector<std::string> excluded_names{};
};

struct WatchChange {
    std::vector<std::string> paths{};
    bool overflow{};
    bool terminal_error{};
    support::Error error{};
};

using WatchChangeSink = std::move_only_function<void(const WatchChange&)>;

class FileWatch {
public:
    virtual ~FileWatch() = default;
    virtual void close() = 0;
    /// How the watcher reports changes; the local adapter is native (inotify).
    virtual std::string_view mode() const = 0;
};

/// Physical operations used by durable tools. Implementations and their callers
/// may run concurrently; the supplied environment must outlive the extension.
class Environment {
public:
    virtual ~Environment() = default;
    virtual support::Expected<std::string> read(std::string_view path, std::stop_token stop) = 0;
    /// Up to `length` bytes at `offset`; fewer only at end of file. An offset at
    /// or past the end reads an empty result, not an error, and nothing before
    /// `offset` is read (frozen pi `BinaryReader.read`).
    virtual support::Expected<std::string> read(
            std::string_view path, std::uintmax_t offset, std::size_t length, std::stop_token stop) = 0;
    virtual support::ExpectedVoid write(std::string_view path, std::string_view content, std::stop_token stop) = 0;
    virtual support::ExpectedVoid append(std::string_view path, std::string_view content, std::stop_token stop) = 0;
    virtual support::Expected<FileInfo> stat(std::string_view path, std::stop_token stop) = 0;
    /// Open a directory for paged listing; no entry is read until `next` is called.
    virtual support::Expected<std::unique_ptr<DirectoryReader>> open_directory(
            std::string_view path, std::stop_token stop) = 0;
    virtual support::Expected<std::vector<FileInfo>> list(std::string_view path, std::stop_token stop) = 0;
    /// Create a fresh temporary directory for one run's output files. An
    /// environment without a temporary area reports that explicitly instead of
    /// silently writing beside the workspace.
    virtual support::Expected<std::string> create_temp_dir(std::string_view prefix, std::stop_token stop) = 0;
    /// The callback receives decoded output as it arrives; retained result fields
    /// are independently bounded. No spill file is created.
    virtual support::Expected<ShellResult> exec(const ShellCommand& command,
            std::chrono::milliseconds timeout,
            OutputSink output,
            std::stop_token stop,
            ShellOutputWindow window = {}) = 0;
    /// Watch one file or directory. Recursive watches cover the whole subtree, add watches
    /// for subdirectories created later, and never follow symbolic links below the target.
    /// Coverage is established before watch() returns; after close() no callback starts.
    /// The sink receives {overflow:true} when coverage becomes uncertain and a terminal
    /// {error} once the watcher stops; no calls follow a terminal error.
    virtual support::Expected<std::unique_ptr<FileWatch>> watch(
            std::string_view path, WatchChangeSink on_change, WatchOptions options = {}) = 0;
};

/// Construct the durable-owned Linux environment rooted at cwd.
[[nodiscard]] std::shared_ptr<Environment> local_environment(std::string cwd);

/// Image capability the coding tools expose to the read tool. Without an
/// `images` processor, inline images within the limits pass through and other
/// images are explicit unsupported-image results.
struct CodingToolOptions {
    std::unique_ptr<ImageProcessor> images{};
    /// Whether the conversation model takes images. Callers set this from the
    /// conversation model; the fail-closed default assumes no image input. A
    /// model without vision still receives the image block and a note
    /// (frozen pi).
    bool vision{false};
    ImageLimits limits{};
};

/// Construct the explicit coding-tools extension; no automatic installation.
[[nodiscard]] Extension coding_tools(std::shared_ptr<Environment> environment, CodingToolOptions options = {});

} // namespace cch::durable
