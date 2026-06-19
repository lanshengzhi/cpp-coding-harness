#include "LocalExecutionEnv.hpp"

#include "../util/OutputLimiter.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
extern char** environ;
#endif

namespace cch::harness {
namespace {

std::size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        ++pos;
    }
    return count;
}

bool secret_env_name(std::string name, const std::vector<std::string>& explicit_secret_names = {}) {
    for (const auto& explicit_name : explicit_secret_names) {
        if (name == explicit_name) {
            return true;
        }
    }
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return name.find("API_KEY") != std::string::npos || name.find("TOKEN") != std::string::npos ||
           name.find("SECRET") != std::string::npos || name.find("PASSWORD") != std::string::npos ||
           name.find("CREDENTIAL") != std::string::npos || name.find("PRIVATE_KEY") != std::string::npos ||
           name.find("AUTH") != std::string::npos || name.find("JWT") != std::string::npos ||
           name.find("CERTIFICATE") != std::string::npos || name.find("PASSPHRASE") != std::string::npos ||
           name.find("OPENAI") != std::string::npos;
}

std::map<std::string, std::string> sanitized_environment(const std::vector<std::string>& explicit_secret_names = {}) {
    std::map<std::string, std::string> env;
#if defined(__unix__) || defined(__APPLE__)
    for (char** current = environ; current != nullptr && *current != nullptr; ++current) {
        std::string entry(*current);
        auto split = entry.find('=');
        if (split == std::string::npos) {
            continue;
        }
        auto key = entry.substr(0, split);
        if (!secret_env_name(key, explicit_secret_names)) {
            env[key] = entry.substr(split + 1);
        }
    }
#else
    (void)explicit_secret_names;
#endif
    return env;
}

[[nodiscard]] util::Error workspace_error(std::string message) {
    return util::make_error(util::ErrorCode::Workspace, message, message);
}

[[nodiscard]] util::Error process_error(std::string message) {
    return util::make_error(util::ErrorCode::Process, message, message);
}

} // namespace

LocalExecutionEnv::LocalExecutionEnv(
    std::filesystem::path workspace,
    bool bash_enabled,
    std::vector<std::string> secret_environment_names,
    std::shared_ptr<util::ProcessRunner> runner)
    : workspace_(std::move(workspace)),
      bash_enabled_(bash_enabled),
      secret_environment_names_(std::move(secret_environment_names)),
      runner_(std::move(runner)),
      fs_(workspace_) {}

// ---------------------------------------------------------------------------
// Tool-shaped methods (compatibility)
// ---------------------------------------------------------------------------

util::Expected<AsyncFileReadResult> LocalExecutionEnv::read_file(std::string path, int offset, int limit) {
    auto content = fs_.read_existing_file(path);
    if (!content) {
        return std::unexpected(content.error());
    }
    offset = std::max(1, offset);
    util::OutputLimit output_limit;
    AsyncFileReadResult result;
    std::string line;
    std::size_t bytes = 0;
    std::size_t lines = 0;
    int line_number = 1;
    int emitted = 0;
    std::istringstream input(*content);
    while (std::getline(input, line)) {
        if (line_number++ < offset) {
            continue;
        }
        if (limit > 0 && emitted >= limit) {
            break;
        }
        const auto next_bytes = bytes + line.size() + 1;
        if (lines >= output_limit.max_lines || next_bytes > output_limit.max_bytes) {
            result.truncated = true;
            break;
        }
        result.content += line;
        result.content += '\n';
        bytes = next_bytes;
        ++lines;
        ++emitted;
    }
    if (!result.content.empty()) {
        result.content.pop_back();
    }
    if (result.truncated) {
        result.content += "\n[output truncated]";
    }
    return result;
}

util::Expected<AsyncFileWriteResult> LocalExecutionEnv::write_file(std::string path, std::string content, bool create_parents) {
    auto written = fs_.write_file(path, content, create_parents);
    if (!written) {
        return std::unexpected(written.error());
    }
    return AsyncFileWriteResult{*written};
}

util::Expected<AsyncFileEditResult> LocalExecutionEnv::edit_file(std::string path, std::string old_text, std::string new_text) {
    auto existing = fs_.read_existing_file(path);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    auto content = std::move(*existing);
    const auto matches = count_occurrences(content, old_text);
    if (matches == 0) {
        return std::unexpected(workspace_error("old_text did not match any text in " + path));
    }
    if (matches > 1) {
        return std::unexpected(workspace_error("old_text matched multiple regions in " + path + "; edit is ambiguous"));
    }
    const auto pos = content.find(old_text);
    content.replace(pos, old_text.size(), new_text);
    auto written = fs_.write_file(path, content, false);
    if (!written) {
        return std::unexpected(written.error());
    }
    return AsyncFileEditResult{old_text.substr(0, 80), new_text.substr(0, 80)};
}

// ---------------------------------------------------------------------------
// Shell methods (compatibility)
// ---------------------------------------------------------------------------

util::Expected<util::ProcessRequest> LocalExecutionEnv::make_shell_request(
    std::string command,
    std::chrono::milliseconds timeout) const {
    if (!bash_enabled_) {
        return std::unexpected(process_error("bash is disabled by default; rerun with explicit bash enablement"));
    }
    util::ProcessRequest request;
    request.command = std::move(command);
    request.working_directory = workspace_;
    request.timeout = timeout;
    request.environment = sanitized_environment(secret_environment_names_);
    request.use_explicit_environment = true;
    return request;
}

AsyncShellResult LocalExecutionEnv::shell_result_from_process(const util::ProcessResult& process) const {
    auto limited = util::limit_output(process.output);
    AsyncShellResult result;
    result.exit_code = process.exit_code;
    result.output = limited.text;
    result.timed_out = process.timed_out;
    return result;
}

util::Expected<AsyncShellResult> LocalExecutionEnv::run_shell(std::string command, std::chrono::milliseconds timeout) {
    auto request = make_shell_request(std::move(command), timeout);
    if (!request) {
        return std::unexpected(request.error());
    }

    boost::asio::io_context io;
    std::optional<util::Expected<util::ProcessResult>> process;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            process = co_await runner_->run(std::move(*request));
            co_return;
        },
        boost::asio::detached);
    io.run();

    if (!process) {
        return std::unexpected(process_error("process execution did not complete"));
    }
    if (!*process) {
        return std::unexpected((*process).error());
    }
    return shell_result_from_process(**process);
}

// ---------------------------------------------------------------------------
// Pi-shaped filesystem methods
// ---------------------------------------------------------------------------

std::expected<std::string, FileError> LocalExecutionEnv::absolutePath(const std::string& path) const {
    return fs_.absolutePath(path);
}

std::expected<std::string, FileError> LocalExecutionEnv::joinPath(const std::vector<std::string>& parts) const {
    return fs_.joinPath(parts);
}

std::expected<std::string, FileError> LocalExecutionEnv::readTextFile(const std::string& path) const {
    return fs_.readTextFile(path);
}

std::expected<std::vector<std::string>, FileError> LocalExecutionEnv::readTextLines(
    const std::string& path,
    std::optional<int> maxLines) const {
    return fs_.readTextLines(path, maxLines);
}

std::expected<BinaryData, FileError> LocalExecutionEnv::readBinaryFile(const std::string& path) const {
    return fs_.readBinaryFile(path);
}

std::expected<void, FileError> LocalExecutionEnv::writeFile(const std::string& path, const WriteContent& content) const {
    return fs_.writeFile(path, content);
}

std::expected<void, FileError> LocalExecutionEnv::appendFile(const std::string& path, const WriteContent& content) const {
    return fs_.appendFile(path, content);
}

std::expected<FileInfo, FileError> LocalExecutionEnv::fileInfo(const std::string& path) const {
    return fs_.fileInfo(path);
}

std::expected<std::vector<FileInfo>, FileError> LocalExecutionEnv::listDir(const std::string& path) const {
    return fs_.listDir(path);
}

std::expected<std::string, FileError> LocalExecutionEnv::canonicalPath(const std::string& path) const {
    return fs_.canonicalPath(path);
}

std::expected<bool, FileError> LocalExecutionEnv::exists(const std::string& path) const {
    return fs_.exists(path);
}

std::expected<void, FileError> LocalExecutionEnv::createDir(const std::string& path, bool recursive) const {
    return fs_.createDir(path, recursive);
}

std::expected<void, FileError> LocalExecutionEnv::remove(const std::string& path, bool recursive) const {
    return fs_.remove(path, recursive);
}

std::expected<std::string, FileError> LocalExecutionEnv::createTempDir(std::optional<std::string> prefix) const {
    return fs_.createTempDir(prefix);
}

std::expected<std::string, FileError> LocalExecutionEnv::createTempFile(
    std::optional<std::string> prefix,
    std::optional<std::string> suffix) const {
    return fs_.createTempFile(prefix, suffix);
}

} // namespace cch::harness
