#pragma once

#include "../ChatClient.hpp"
#include "OpenAICompletionsCompat.hpp"
#include "StreamTransport.hpp"

#include <chrono>
#include <memory>
#include <string>

namespace cch::ai::providers {

struct OpenAIStreamConfig {
    std::string base_url{"https://api.openai.com"};
    std::string api_key;
    std::string api_key_env{"OPENAI_API_KEY"};
    std::string model{"gpt-4.1-mini"};
    std::string organization;
    std::string project;
    std::chrono::milliseconds timeout{30000};
    OpenAICompletionsCompat compat;
};

class StreamingOpenAIChatClient final : public ai::StreamingChatClient {
public:
    StreamingOpenAIChatClient(std::shared_ptr<StreamTransport> transport, OpenAIStreamConfig config);

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink sink) override;

private:
    [[nodiscard]] util::Expected<std::string> resolve_api_key() const;
    [[nodiscard]] std::string completions_url() const;

    std::shared_ptr<StreamTransport> transport_;
    OpenAIStreamConfig config_;
};

} // namespace cch::ai::providers
