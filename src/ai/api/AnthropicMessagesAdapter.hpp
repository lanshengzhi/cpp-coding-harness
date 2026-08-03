#pragma once

#include <cch/ai/Provider.hpp>
#include <cch/ai/providers/StreamTransport.hpp>

#include <memory>

namespace cch::ai::api {

/// Private protocol executor for the frozen `anthropic-messages` Kimi surface.
/// Provider composition owns registration; callers select it only through a
/// Model whose api identity is `anthropic-messages`.
class AnthropicMessagesAdapter final {
public:
    explicit AnthropicMessagesAdapter(
        std::shared_ptr<providers::StreamTransport> transport);
    AnthropicMessagesAdapter(AnthropicMessagesAdapter&&) noexcept;
    AnthropicMessagesAdapter& operator=(AnthropicMessagesAdapter&&) noexcept;
    ~AnthropicMessagesAdapter();
    AnthropicMessagesAdapter(const AnthropicMessagesAdapter&) = delete;
    AnthropicMessagesAdapter& operator=(const AnthropicMessagesAdapter&) = delete;

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
