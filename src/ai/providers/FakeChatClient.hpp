#pragma once

#include <memory>

namespace cch::ai {
class StreamingChatClient;
}

namespace cch::ai::providers {

[[nodiscard]] std::unique_ptr<ai::StreamingChatClient> make_scripted_fake_chat_client();

} // namespace cch::ai::providers
