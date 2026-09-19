#include "OpenAICodexResponsesAdapter.hpp"

#include "MessageConversion.hpp"
#include "ai/Headers.hpp"
#include "ai/Timestamps.hpp"
#include "ai/api/PartialJson.hpp"
#include "ai/api/ResponsesEventProcessor.hpp"
#include "ai/auth/Pkce.hpp"
#include "ai/providers/ProviderError.hpp"
#include "ai/providers/RetryPolicy.hpp"
#include "ai/providers/SseParser.hpp"
#include "ai/providers/StreamEmit.hpp"
#include "ai/providers/StreamExecutionEngine.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "CodexShared.hpp"

#include "ai/auth/Pkce.hpp"

namespace cch::ai::api {

[[nodiscard]] std::string resolve_codex_url(std::string_view base_url) {
    std::string raw = base_url.empty() ? std::string{kDefaultCodexBaseUrl} : std::string{base_url};
    while (!raw.empty() && raw.back() == '/') {
        raw.pop_back();
    }
    if (raw.ends_with("/codex/responses")) {
        return raw;
    }
    if (raw.ends_with("/codex")) {
        return raw + "/responses";
    }
    return raw + "/codex/responses";
}

[[nodiscard]] std::string resolve_codex_websocket_url(std::string_view base_url) {
    std::string url = resolve_codex_url(base_url);
    if (url.starts_with("https://")) {
        url.replace(0, 8, "wss://");
    } else if (url.starts_with("http://")) {
        url.replace(0, 7, "ws://");
    }
    return url;
}

/// pi extractAccountId: the Codex token is a ChatGPT JWT whose payload carries
/// `https://api.openai.com/auth.chatgpt_account_id`. The account id keys the
/// per-session socket cache and the `chatgpt-account-id` request header. The
/// decode itself is the shared auth JWT walk; failures collapse to the one
/// Codex-facing stream error.
[[nodiscard]] support::Expected<std::string> extract_account_id(std::string_view token) {
    auto account = auth::extract_account_id(token);
    if (!account) {
        return std::unexpected(providers::make_stream_error("Failed to extract accountId from token"));
    }
    return std::move(*account);
}

/// pi buildBaseCodexHeaders plus the per-transport protocol fields. Base
/// headers are the post-merge/post-transform ModelAuth headers; deletions the
/// transform performed are honored for optional protocol fields.
[[nodiscard]] ProviderHeaders codex_headers(
        const ProviderStreamOptions& options, std::string_view account_id, bool websocket) {
    ProviderHeaders headers = options.auth.headers;
    if (options.auth.api_key && !options.auth.api_key->empty()) {
        set_header(headers, "Authorization", "Bearer " + *options.auth.api_key);
    }
    set_header(headers, "chatgpt-account-id", std::string{account_id});
    set_header(headers, "originator", "pi");
    set_header(headers, "User-Agent", "pi (cpp-harness)");
    if (websocket) {
        erase_header(headers, "accept");
        erase_header(headers, "content-type");
        erase_header(headers, "openai-beta");
        // pi's connectWebSocket deletes the OpenAI-Beta header before the
        // handshake, so the WS connection deliberately carries no beta value.
        return headers;
    }
    set_header(headers, "OpenAI-Beta", std::string{kSseBeta});
    if (!has_header(headers, "accept") && !header_deleted(options, "accept")) {
        set_header(headers, "accept", "text/event-stream");
    }
    if (!has_header(headers, "content-type") && !header_deleted(options, "content-type")) {
        set_header(headers, "content-type", "application/json");
    }
    return headers;
}

enum class WsFrameAction { Continue, Terminal };

} // namespace cch::ai::api
