#pragma once

#include "../../src/ai/ChatClient.hpp"
#include "../../src/llm/ChatClient.hpp"

#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace cch::tests {

class FakeChatClient : public ai::ChatClient {
public:
    void push_response(llm::ChatResponse response) { responses_.push_back(std::move(response)); }
    void fail_next(std::string error) { failures_.push_back(std::move(error)); }

    util::Result<ai::ChatResponse> complete(const ai::ChatRequest& request) override {
        requests.push_back(llm::chat_request_from_ai(request));
        if (!failures_.empty()) {
            auto error = failures_.front();
            failures_.pop_front();
            return util::Result<ai::ChatResponse>::failure(error);
        }
        if (responses_.empty()) {
            llm::ChatResponse response;
            response.assistant_message.role = agent::Role::Assistant;
            response.assistant_message.content = "fake response";
            return util::Result<ai::ChatResponse>::success(llm::to_ai_chat_response(response));
        }
        auto response = responses_.front();
        responses_.pop_front();
        return util::Result<ai::ChatResponse>::success(llm::to_ai_chat_response(std::move(response)));
    }

    std::vector<llm::ChatRequest> requests;

private:
    std::deque<llm::ChatResponse> responses_;
    std::deque<std::string> failures_;
};

inline llm::ChatResponse text_response(std::string text) {
    llm::ChatResponse response;
    response.assistant_message.role = agent::Role::Assistant;
    response.assistant_message.content = std::move(text);
    response.stop_reason = "stop";
    return response;
}

inline llm::ChatResponse tool_response(std::string id, std::string name, boost::json::object arguments, std::string text = {}) {
    llm::ChatResponse response;
    response.assistant_message.role = agent::Role::Assistant;
    response.assistant_message.content = std::move(text);
    agent::ToolCall call;
    call.id = std::move(id);
    call.name = std::move(name);
    call.arguments = std::move(arguments);
    call.raw_arguments = boost::json::serialize(call.arguments);
    response.assistant_message.tool_calls.push_back(std::move(call));
    response.stop_reason = "tool_calls";
    return response;
}

} // namespace cch::tests
