#pragma once

#include <cch/ai/Auth.hpp>
#include <cch/util/Error.hpp>
#include "util/ExpectedMacros.hpp"

#include <boost/asio/awaitable.hpp>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace cch::coding_agent {

/// Ambient-auth capability backed by the process environment and filesystem.
/// Used by the default-created ModelRuntime; the ai layer never reads process
/// globals directly.
class ProcessAuthContext final : public ai::AuthContext {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<std::string>>> environment(
        std::string name) const override {
        const char* value = std::getenv(name.c_str());
        if (value == nullptr || *value == '\0') {
            co_return std::optional<std::string>{};
        }
        co_return std::optional<std::string>{std::string{value}};
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<bool>> file_exists(
        std::string path) const override {
        if (path.starts_with("~/")) {
            if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
                path = (std::filesystem::path{home} / path.substr(2)).string();
            }
        }
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Auth,
                "could not inspect authentication file",
                error.message()));
        }
        co_return exists;
    }
};

} // namespace cch::coding_agent
