#pragma once

#include <cch/ai/Auth.hpp>
#include <cch/support/Error.hpp>

#include <cstdlib>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace cch::coding_agent {

/// Ambient-auth capability backed by the process environment and filesystem.
/// Used by the default-created ModelRuntime; the ai layer never reads process
/// globals directly. Both lookups are bounded synchronous value operations, so
/// they complete as ready `AsyncResult` values without support allocation.
class ProcessAuthContext final : public ai::AuthContext {
public:
    [[nodiscard]] cch::support::AsyncResult<std::optional<std::string>> environment(
        std::string name) const override {
        const char* value = std::getenv(name.c_str());
        if (value == nullptr || *value == '\0') {
            return cch::support::AsyncResult<std::optional<std::string>>(
                std::expected<std::optional<std::string>, cch::support::Error>{
                    std::optional<std::string>{}});
        }
        return cch::support::AsyncResult<std::optional<std::string>>(
            std::expected<std::optional<std::string>, cch::support::Error>{
                std::optional<std::string>{std::string{value}}});
    }

    [[nodiscard]] cch::support::AsyncResult<bool> file_exists(
        std::string path) const override {
        if (path.starts_with("~/")) {
            if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
                path = (std::filesystem::path{home} / path.substr(2)).string();
            }
        }
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error) {
            return cch::support::AsyncResult<bool>(
                std::unexpected(cch::support::make_error(
                    cch::support::ErrorCode::Auth,
                    "could not inspect authentication file",
                    error.message())));
        }
        return cch::support::AsyncResult<bool>(
            std::expected<bool, cch::support::Error>{exists});
    }
};

} // namespace cch::coding_agent
