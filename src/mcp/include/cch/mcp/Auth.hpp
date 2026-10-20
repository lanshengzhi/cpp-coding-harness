#pragma once

// MCP OAuth presentation values and the shared credential/namespace facts
// (pi `packages/mcp/src/oauth/types.ts` plus the application's auth
// interaction shape). Durable token state is supplied by the caller through
// `OAuthStateStore`; these values never enter model requests or history.

#include <cch/support/AsyncResult.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>

namespace cch::mcp {

/// MCP token state used during authorization and refresh.
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

/// pi `mcpNamespace`: `mcp__<name>` with `-` replaced by `_`. Two names that
/// differ only in `-`/`_` share this namespace and cannot coexist.
[[nodiscard]] inline std::string mcp_namespace(std::string_view name) {
    std::string result = "mcp__";
    result.reserve(name.size() + 5);
    for (const char character : name) {
        result.push_back(character == '-' ? '_' : character);
    }
    return result;
}

/// The namespaced key used to identify one MCP server's OAuth credential
/// (pi `mcp__<server>`, `-` replaced by `_`).
[[nodiscard]] inline std::string mcp_oauth_provider_id(std::string_view server_name) {
    return mcp_namespace(server_name);
}

/// Expired or absent MCP credentials require server sign-in.
[[nodiscard]] inline std::string format_oauth_reauthenticate_message(std::string_view server) {
    return "Authentication failed for \"" + std::string{server} +
           "\". Credentials may have expired or network is unavailable. "
           "Run '/mcp' to re-authenticate " +
           std::string{server} + ".";
}

} // namespace cch::mcp
