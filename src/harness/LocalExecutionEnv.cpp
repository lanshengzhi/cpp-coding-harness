#include "LocalExecutionEnv.hpp"

#include "../tools/AtomicWrite.hpp"
#include "../tools/OutputLimiter.hpp"
#include "../tools/PathGuard.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
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
      runner_(std::move(runner)) {}

util::Expected<AsyncFileReadResult> LocalExecutionEnv::read_file(std::string path, int offset, int limit) {
    auto guard = tools::PathGuard::create(workspace_);
    if (!guard) {
        return std::unexpected(guard.error());
    }
    auto resolved = guard->resolve_existing_file(path);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    std::ifstream input(*resolved, std::ios::binary);
    if (!input) {
        return std::unexpected(workspace_error("could not open file for reading: " + path));
    }
    offset = std::max(1, offset);
    tools::OutputLimit output_limit;
    AsyncFileReadResult result;
    std::string line;
    std::size_t bytes = 0;
    std::size_t lines = 0;
    int line_number = 1;
    int emitted = 0;
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
    auto guard = tools::PathGuard::create(workspace_);
    if (!guard) {
        return std::unexpected(guard.error());
    }
    auto resolved = guard->resolve_for_write(path, create_parents);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    auto written = tools::write_atomic_file(*resolved, content);
    if (!written) {
        return std::unexpected(written.error());
    }
    return AsyncFileWriteResult{content.size()};
}

util::Expected<AsyncFileEditResult> LocalExecutionEnv::edit_file(std::string path, std::string old_text, std::string new_text) {
    auto guard = tools::PathGuard::create(workspace_);
    if (!guard) {
        return std::unexpected(guard.error());
    }
    auto resolved = guard->resolve_existing_file(path);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    std::ifstream input(*resolved, std::ios::binary);
    if (!input) {
        return std::unexpected(workspace_error("could not open file for editing: " + path));
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        return std::unexpected(workspace_error("could not read file for editing: " + path));
    }
    auto content = buffer.str();
    const auto matches = count_occurrences(content, old_text);
    if (matches == 0) {
        return std::unexpected(workspace_error("old_text did not match any text in " + path));
    }
    if (matches > 1) {
        return std::unexpected(workspace_error("old_text matched multiple regions in " + path + "; edit is ambiguous"));
    }
    const auto pos = content.find(old_text);
    content.replace(pos, old_text.size(), new_text);
    auto written = tools::write_atomic_file(*resolved, content);
    if (!written) {
        return std::unexpected(written.error());
    }
    return AsyncFileEditResult{old_text.substr(0, 80), new_text.substr(0, 80)};
}

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
    auto limited = tools::limit_output(process.output);
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

} // namespace cch::harness
