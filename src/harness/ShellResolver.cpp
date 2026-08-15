#include "ShellResolver.hpp"

#include "ai/BoundedText.hpp"
#include "harness/OutputLimiter.hpp"

#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::harness {
namespace {

[[nodiscard]] bool executable_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error || !std::filesystem::is_regular_file(status)) {
        return false;
    }
    return ::access(path.c_str(), X_OK) == 0;
}

[[nodiscard]] std::filesystem::path workspace_relative(
    std::filesystem::path path,
    const std::filesystem::path& workspace) {
    if (path.is_relative()) {
        path = workspace / path;
    }
    return path.lexically_normal();
}

[[nodiscard]] std::optional<std::filesystem::path> user_home_directory() {
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path{home};
    }

    const long configured_size = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    const auto buffer_size = configured_size > 0
        ? static_cast<std::size_t>(configured_size)
        : std::size_t{16 * 1024};
    std::vector<char> buffer(buffer_size);
    struct passwd entry{};
    struct passwd* result = nullptr;
    if (::getpwuid_r(::getuid(), &entry, buffer.data(), buffer.size(), &result) == 0 &&
        result != nullptr && result->pw_dir != nullptr && result->pw_dir[0] != '\0') {
        return std::filesystem::path{result->pw_dir};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::filesystem::path> expand_leading_home(
    std::string path,
    const std::filesystem::path& workspace) {
    if (path == "~" || path.starts_with("~/")) {
        auto home = user_home_directory();
        if (!home) {
            return std::nullopt;
        }
        path = home->string() + path.substr(1);
    }
    return workspace_relative(std::filesystem::path{std::move(path)}, workspace);
}

[[nodiscard]] std::optional<std::filesystem::path> search_path(
    std::string_view executable,
    const std::filesystem::path& workspace,
    const std::map<std::string, std::string>& environment) {
    const auto path_entry = environment.find("PATH");
    if (path_entry == environment.end()) {
        return std::nullopt;
    }

    const std::string_view path_value = path_entry->second;
    std::size_t start = 0;
    while (start <= path_value.size()) {
        const auto separator = path_value.find(':', start);
        const auto end = separator == std::string_view::npos ? path_value.size() : separator;
        const auto directory = path_value.substr(start, end - start);
        if (!directory.empty()) {
            auto candidate = workspace_relative(
                std::filesystem::path{directory} / executable,
                workspace);
            if (executable_file(candidate)) {
                return candidate;
            }
        }
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1;
    }
    return std::nullopt;
}

[[nodiscard]] ExecutionError shell_unavailable(std::string message) {
    return ExecutionError{
        .code = ExecutionErrorCode::ShellUnavailable,
        .message = std::move(message),
    };
}

} // namespace

std::expected<std::filesystem::path, ExecutionError> resolve_shell_executable(
    const std::optional<std::string>& configured_path,
    const std::filesystem::path& workspace,
    const std::map<std::string, std::string>& environment,
    const std::filesystem::path& system_bash_candidate) {
    if (configured_path && !configured_path->empty()) {
        const auto expanded = expand_leading_home(*configured_path, workspace);
        if (!expanded) {
            return std::unexpected(shell_unavailable(
                "could not expand the configured shell home directory"));
        }
        if (executable_file(*expanded)) {
            return *expanded;
        }
        const harness::OutputLimit output_limit;
        return std::unexpected(shell_unavailable(
            "configured shell path is unavailable or not executable: " +
            ai::bounded_redacted_text(
                expanded->string(),
                output_limit.max_bytes,
                "...[truncated]")));
    }

    if (executable_file(system_bash_candidate)) {
        return system_bash_candidate;
    }
    if (auto bash = search_path("bash", workspace, environment)) {
        return *bash;
    }
    if (auto sh = search_path("sh", workspace, environment)) {
        return *sh;
    }
    return std::unexpected(shell_unavailable(
        "no usable shell found: tried /bin/bash, PATH bash, and PATH sh"));
}

} // namespace cch::harness
