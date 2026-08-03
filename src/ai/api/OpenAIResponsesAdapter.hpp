#pragma once

#include <cch/ai/Provider.hpp>
#include <cch/ai/providers/StreamTransport.hpp>

#include <memory>

namespace cch::ai::api {

/// Private protocol executor for the frozen `openai-responses` surface.
/// Provider composition owns registration; callers select it only through a
/// Model whose api identity is `openai-responses`.
class OpenAIResponsesAdapter final {
public:
    explicit OpenAIResponsesAdapter(
        std::shared_ptr<providers::StreamTransport> transport);
    OpenAIResponsesAdapter(OpenAIResponsesAdapter&&) noexcept;
    OpenAIResponsesAdapter& operator=(OpenAIResponsesAdapter&&) noexcept;
    ~OpenAIResponsesAdapter();
    OpenAIResponsesAdapter(const OpenAIResponsesAdapter&) = delete;
    OpenAIResponsesAdapter& operator=(const OpenAIResponsesAdapter&) = delete;

    /// Borrowed model and context must outlive the returned awaitable.
    [[nodiscard]] boost::asio::awaitable<util::Expected<AssistantMessage>> stream(
        const Model& model,
        const AiContext& context,
        ProviderStreamOptions options,
        AssistantEventSink sink);

private:
    std::shared_ptr<providers::StreamTransport> transport_;
};

} // namespace cch::ai::api
