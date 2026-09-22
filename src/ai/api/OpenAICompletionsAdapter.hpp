#pragma once

#include "ai/providers/Provider.hpp"
#include "ai/providers/StreamTransport.hpp"

#include <memory>

namespace cch::ai::api {

/// Private protocol executor for the `openai-completions` Chat Completions
/// surface. Provider composition owns registration; callers select it only
/// through a Model whose api identity is `openai-completions`.
///
/// Stateless per call: the shared transport is driven by the calling
/// single-threaded executor (StreamTransport executor contract).
class OpenAICompletionsAdapter final {
public:
    explicit OpenAICompletionsAdapter(
            std::shared_ptr<providers::StreamTransport> transport);
    OpenAICompletionsAdapter(OpenAICompletionsAdapter&&) noexcept;
    OpenAICompletionsAdapter& operator=(OpenAICompletionsAdapter&&) noexcept;
    ~OpenAICompletionsAdapter();
    OpenAICompletionsAdapter(const OpenAICompletionsAdapter&) = delete;
    OpenAICompletionsAdapter& operator=(const OpenAICompletionsAdapter&) = delete;

    /// Borrowed model and context must outlive the returned awaitable.
    [[nodiscard]] boost::asio::awaitable<support::Expected<AssistantMessage>> stream(
            const Model& model,
            const AiContext& context,
            ProviderStreamOptions options,
            AssistantEventSink sink);

private:
    std::shared_ptr<providers::StreamTransport> transport_;
};

} // namespace cch::ai::api
