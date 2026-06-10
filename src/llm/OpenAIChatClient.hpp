#pragma once

#include "ChatClient.hpp"
#include "HttpTransport.hpp"
#include "../ai/providers/OpenAIChatClient.hpp"

#include <memory>

namespace cch::llm {

using OpenAIConfig = ai::providers::OpenAIConfig;

class OpenAIChatClient : public ChatClient {
public:
    OpenAIChatClient(std::shared_ptr<HttpTransport> transport, OpenAIConfig config);

    [[nodiscard]] util::Result<ChatResponse> complete(const ChatRequest& request) override;

private:
    ai::providers::OpenAIChatClient client_;
};

} // namespace cch::llm
