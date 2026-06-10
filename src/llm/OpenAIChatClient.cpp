#include "OpenAIChatClient.hpp"

#include <utility>

namespace cch::llm {

OpenAIChatClient::OpenAIChatClient(std::shared_ptr<HttpTransport> transport, OpenAIConfig config)
    : client_(std::move(transport), std::move(config)) {}

util::Result<ChatResponse> OpenAIChatClient::complete(const ChatRequest& request) {
    auto response = client_.complete(to_ai_chat_request(request));
    if (!response) {
        return util::Result<ChatResponse>::failure(response.error());
    }
    return util::Result<ChatResponse>::success(chat_response_from_ai(response.value()));
}

} // namespace cch::llm
