#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace cch::ai::auth {

/// Frozen success page body, byte-identical to pi's `oauthSuccessHtml` at
/// baseline 83114817 (`packages/ai/src/auth/oauth/oauth-page.ts`).
[[nodiscard]] std::string oauth_success_html(std::string_view message);

/// Frozen failure page body, byte-identical to pi's `oauthErrorHtml`.
[[nodiscard]] std::string oauth_error_html(
    std::string_view message,
    std::optional<std::string_view> details = std::nullopt);

} // namespace cch::ai::auth
