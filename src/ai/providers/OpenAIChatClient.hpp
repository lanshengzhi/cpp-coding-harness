#pragma once

#include <cch/ai/Auth.hpp>
#include <cch/ai/ChatClient.hpp>
#include <cch/ai/providers/OpenAICompletionsCompat.hpp>
#include <cch/ai/providers/StreamTransport.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace cch::ai::providers {

struct OpenAIStreamConfig {
    std::string organization;
    std::string project;
    std::chrono::milliseconds timeout{30000};
    OpenAICompletionsCompat compat;
};

/// Transitional private wire adapter for the OpenAI-compatible Provider.
/// Request authentication is supplied by Models for every call.
class StreamingOpenAIChatClient final {
public:
    StreamingOpenAIChatClient(
        std::shared_ptr<StreamTransport> transport,
        OpenAIStreamConfig config);

    /// The borrowed request must outlive the returned awaitable.
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::ModelAuth auth,
        ai::AssistantEventSink sink);

private:
    [[nodiscard]] static std::string completions_url(std::string_view base_url);

    std::shared_ptr<StreamTransport> transport_;
    OpenAIStreamConfig config_;
};

} // namespace cch::ai::providers
