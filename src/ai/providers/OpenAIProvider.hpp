#pragma once

#include <cch/ai/Model.hpp>
#include <cch/ai/Provider.hpp>
#include <cch/ai/providers/StreamTransport.hpp>
#include <cch/ai/providers/WebSocketTransport.hpp>
#include "ai/api/OpenAICodexResponsesAdapter.hpp"
#include "ai/providers/OpenAIChatClient.hpp"

#include <memory>
#include <string>
#include <vector>

namespace cch::ai::providers {

/// Compose the transitional OpenAI-compatible Provider around the existing
/// private wire adapter. Authentication is resolved by Models for every call.
[[nodiscard]] std::shared_ptr<ai::Provider> make_openai_compatible_provider(
    std::string provider_id,
    std::vector<ai::Model> models,
    std::vector<std::string> api_key_env,
    std::shared_ptr<StreamTransport> transport,
    OpenAIStreamConfig config = {});

/// Private composition seam for config-only Providers whose Model selects the
/// exact `openai-responses` adapter. #345 makes this path user-reachable.
[[nodiscard]] std::shared_ptr<ai::Provider> make_openai_responses_provider(
    std::string provider_id,
    std::vector<ai::Model> models,
    std::vector<std::string> api_key_env,
    std::shared_ptr<StreamTransport> transport);

/// Private composition seam for the WebSocket-first `openai-codex-responses`
/// adapter. The cache config is injectable so tests can shrink the socket
/// reuse expiry windows.
[[nodiscard]] std::shared_ptr<ai::Provider> make_openai_codex_responses_provider(
    std::string provider_id,
    std::vector<ai::Model> models,
    std::vector<std::string> api_key_env,
    std::shared_ptr<StreamTransport> http_transport,
    std::shared_ptr<WebSocketTransport> ws_transport,
    api::CodexWebSocketCacheConfig cache_config = {});

} // namespace cch::ai::providers
