#pragma once

#include <cch/ai/Provider.hpp>
#include <cch/ai/providers/StreamTransport.hpp>
#include <cch/ai/providers/WebSocketTransport.hpp>

#include <chrono>
#include <memory>

namespace cch::ai::api {

/// Private protocol executor for the frozen `openai-codex-responses` surface
/// (ADR 0033): WebSocket-first transport with SSE fallback, session/account
/// socket reuse, `previous_response_id` continuation, and the two targeted WS
/// retries. Provider composition owns registration; callers select it only
/// through a Model whose api identity is `openai-codex-responses`.
class OpenAICodexResponsesAdapter final {
public:
    explicit OpenAICodexResponsesAdapter(
        std::shared_ptr<providers::StreamTransport> http_transport,
        std::shared_ptr<providers::WebSocketTransport> ws_transport,
        providers::CodexWebSocketCacheConfig cache_config = {});
    OpenAICodexResponsesAdapter(OpenAICodexResponsesAdapter&&) noexcept;
    OpenAICodexResponsesAdapter& operator=(OpenAICodexResponsesAdapter&&) noexcept;
    ~OpenAICodexResponsesAdapter();
    OpenAICodexResponsesAdapter(const OpenAICodexResponsesAdapter&) = delete;
    OpenAICodexResponsesAdapter& operator=(const OpenAICodexResponsesAdapter&) = delete;

    /// Borrowed model and context must outlive the returned awaitable.
    [[nodiscard]] boost::asio::awaitable<util::Expected<AssistantMessage>> stream(
        const Model& model,
        const AiContext& context,
        ProviderStreamOptions options,
        AssistantEventSink sink);

private:
    struct Impl;
    std::shared_ptr<providers::StreamTransport> http_transport_;
    std::shared_ptr<providers::WebSocketTransport> ws_transport_;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::ai::api
