#pragma once

#include "ai/providers/Provider.hpp"
#include "ai/providers/StreamTransport.hpp"
#include "ai/providers/WebSocketTransport.hpp"

#include <chrono>
#include <memory>

namespace cch::ai::api {

/// Private protocol executor for the frozen `openai-codex-responses` surface
/// (ADR 0033): WebSocket-first transport with SSE fallback, session/account
/// socket reuse, `previous_response_id` continuation, and the two targeted WS
/// retries. Provider composition owns registration; callers select it only
/// through a Model whose api identity is `openai-codex-responses`.
///
/// Concurrency contract: the adapter owns the mutable Codex WebSocket session
/// cache and the SSE-fallback set; it is not internally synchronized. All
/// `stream` calls on one adapter must be driven by a single-threaded executor
/// or otherwise serialized — do not run `stream` concurrently on the same
/// adapter from two threads.
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
    [[nodiscard]] boost::asio::awaitable<support::Expected<AssistantMessage>> stream(
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
