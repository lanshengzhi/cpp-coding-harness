#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace cch::coding_agent {

/// pi's `UNKNOWN_PROVIDER` (`packages/coding-agent/src/core/auth-guidance.ts`):
/// the provider id of the placeholder `kDefaultModel` and of unresolvable
/// identities. The no-key guidance renders it as "the selected model".
inline constexpr std::string_view kUnknownProvider = "unknown";

/// Default docs path for the login-help lines of the no-key guidance. pi's
/// `getDocsPath()` resolves `<packageDir>/docs` inside the pi package; the
/// C++ harness cannot discover a pi install, so the session layer formats the
/// guidance with this deterministic user-level root (under the shared
/// `~/.pi` state) instead. The committed re-auth goldens pin this default.
inline constexpr std::string_view kDefaultAuthGuidanceDocsPath = "~/.pi/docs";

/// pi `getProviderLoginHelp` (`auth-guidance.ts`): the verbatim "Use /login"
/// help block, with the two docs-path lines resolved from `docs_path`.
[[nodiscard]] inline std::string get_provider_login_help(
    const std::filesystem::path& docs_path) {
    return "Use /login to log into a provider via OAuth or API key. See:\n"
           "  " +
           (docs_path / "providers.md").string() + "\n" +
           "  " + (docs_path / "models.md").string();
}

/// pi `formatNoApiKeyFoundMessage` (`auth-guidance.ts`), verbatim: the
/// no-key re-auth branch. An `unknown` provider id renders as "the selected
/// model".
[[nodiscard]] inline std::string format_no_api_key_found_message(
    std::string_view provider,
    const std::filesystem::path& docs_path) {
    const std::string_view provider_display =
        provider == kUnknownProvider ? "the selected model" : provider;
    return "No API key found for " + std::string{provider_display} +
           ".\n\n" + get_provider_login_help(docs_path);
}

/// pi `agent-session.ts` `_getRequiredRequestAuth` OAuth branch, verbatim:
/// the missing/expired-credential re-auth guidance including `Run '/login X'`.
[[nodiscard]] inline std::string format_oauth_reauthenticate_message(
    std::string_view provider) {
    return "Authentication failed for \"" + std::string{provider} +
           "\". Credentials may have expired or network is unavailable. "
           "Run '/login " +
           std::string{provider} + "' to re-authenticate.";
}

} // namespace cch::coding_agent
