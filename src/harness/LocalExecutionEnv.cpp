#include "LocalExecutionEnv.hpp"

#include "../tools/AtomicWrite.hpp"
#include "../tools/OutputLimiter.hpp"
#include "../tools/PathGuard.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
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
        pos += needle.size();
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

util::Result<FileReadResult> LocalExecutionEnv::read_file(const std::string& path, int offset, int limit) {
    auto guard = tools::PathGuard::create(workspace_);
    if (!guard) {
        return util::Result<FileReadResult>::failure(guard.error());
    }
    auto resolved = guard.value().resolve_existing_file(path);
    if (!resolved) {
        return util::Result<FileReadResult>::failure(resolved.error());
    }
    std::ifstream input(resolved.value(), std::ios::binary);
    if (!input) {
        return util::Result<FileReadResult>::failure("could not open file for reading: " + path);
    }
    offset = std::max(1, offset);
    tools::OutputLimit output_limit;
    FileReadResult result;
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
    return util::Result<FileReadResult>::success(std::move(result));
}

util::Result<FileWriteResult> LocalExecutionEnv::write_file(const std::string& path, const std::string& content, bool create_parents) {
    auto guard = tools::PathGuard::create(workspace_);
    if (!guard) {
        return util::Result<FileWriteResult>::failure(guard.error());
    }
    auto resolved = guard.value().resolve_for_write(path, create_parents);
    if (!resolved) {
        return util::Result<FileWriteResult>::failure(resolved.error());
    }
    auto written = tools::write_atomic_file(resolved.value(), content);
    if (!written) {
        return util::Result<FileWriteResult>::failure(written.error() + ": " + path);
    }
    return util::Result<FileWriteResult>::success(FileWriteResult{content.size()});
}

util::Result<FileEditResult> LocalExecutionEnv::edit_file(const std::string& path, const std::string& old_text, const std::string& new_text) {
    auto guard = tools::PathGuard::create(workspace_);
    if (!guard) {
        return util::Result<FileEditResult>::failure(guard.error());
    }
    auto resolved = guard.value().resolve_existing_file(path);
    if (!resolved) {
        return util::Result<FileEditResult>::failure(resolved.error());
    }
    std::ifstream input(resolved.value(), std::ios::binary);
    if (!input) {
        return util::Result<FileEditResult>::failure("could not open file for editing: " + path);
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    auto content = buffer.str();
    const auto matches = count_occurrences(content, old_text);
    if (matches == 0) {
        return util::Result<FileEditResult>::failure("old_text did not match any text in " + path);
    }
    if (matches > 1) {
        return util::Result<FileEditResult>::failure("old_text matched multiple regions in " + path + "; edit is ambiguous");
    }
    const auto pos = content.find(old_text);
    content.replace(pos, old_text.size(), new_text);
    auto written = tools::write_atomic_file(resolved.value(), content);
    if (!written) {
        return util::Result<FileEditResult>::failure(written.error() + ": " + path);
    }
    return util::Result<FileEditResult>::success(FileEditResult{old_text.substr(0, 80), new_text.substr(0, 80)});
}

util::Result<ShellResult> LocalExecutionEnv::run_shell(const std::string& command, std::chrono::milliseconds timeout) {
    if (!bash_enabled_) {
        return util::Result<ShellResult>::failure("bash is disabled by default; rerun with explicit bash enablement");
    }
    util::ProcessRequest request;
    request.command = command;
    request.working_directory = workspace_;
    request.timeout = timeout;
    request.environment = sanitized_environment(secret_environment_names_);
    request.use_explicit_environment = true;
    auto process = runner_->run(request);
    if (!process) {
        return util::Result<ShellResult>::failure(process.error());
    }
    auto limited = tools::limit_output(process.value().output);
    ShellResult result;
    result.exit_code = process.value().exit_code;
    result.output = limited.text;
    result.timed_out = process.value().timed_out;
    return util::Result<ShellResult>::success(std::move(result));
}

} // namespace cch::harness
