#pragma once

#include <cch/support/AsyncResult.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>

namespace cch::coding_agent::mcp {

/// MCP token state used during authorization and refresh. Durable state is
/// owned by McpAuthStore; these values never enter model requests or history.
struct OAuthCredential {
    std::string refresh{};
    std::string access{};
    std::int64_t expires{0};
    std::optional<std::string> client_id{std::nullopt};
};

/// Manual redirect URL requested while the loopback callback is pending.
/// The prompt token cancels this prompt when the callback completes first.
struct AuthPrompt {
    std::string message{};
    std::optional<std::string> placeholder{std::nullopt};
    std::optional<std::stop_token> stop_token{std::nullopt};
};

struct AuthUrl {
    std::string url{};
    std::optional<std::string> instructions{std::nullopt};
};

struct AuthProgress {
    std::string message{};
};

/// Best-effort presentation values contain no credentials or authorization codes.
using AuthEventVariant = std::variant<AuthProgress, AuthUrl>;

struct AuthEvent {
    AuthEventVariant kind{AuthProgress{}};
};

using AuthPromptHook = std::move_only_function<support::AsyncResult<std::string>(AuthPrompt)>;
using AuthNotifyHook = std::move_only_function<void(const AuthEvent&)>;

struct AuthInteraction {
    std::stop_token stop_token{};
    AuthPromptHook prompt{};
    AuthNotifyHook notify{};
};

/// Expired or absent MCP credentials require server sign-in through /mcp.
[[nodiscard]] inline std::string format_oauth_reauthenticate_message(std::string_view server) {
    return "Authentication failed for \"" + std::string{server} +
           "\". Credentials may have expired or network is unavailable. "
           "Run '/mcp' to re-authenticate " +
           std::string{server} + ".";
}

} // namespace cch::coding_agent::mcp
