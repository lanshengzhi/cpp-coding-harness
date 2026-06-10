#pragma once

#include "Context.hpp"
#include "StopReason.hpp"
#include "../util/Result.hpp"

namespace cch::ai {

struct Usage {
    int input_tokens{0};
    int output_tokens{0};
};

struct ChatRequest {
    Context context;
};

struct ChatResponse {
    Message assistant_message;
    StopReason stop_reason{StopReason::Stop};
    Usage usage;
};

class ChatClient {
public:
    virtual ~ChatClient() = default;
    [[nodiscard]] virtual util::Result<ChatResponse> complete(const ChatRequest& request) = 0;
};

} // namespace cch::ai
