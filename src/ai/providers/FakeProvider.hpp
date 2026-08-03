#pragma once

#include <cch/util/Error.hpp>

#include <memory>
#include <string>

namespace cch::ai {
class Models;
class Provider;
class StreamingChatClient;
}

namespace cch::ai::providers {

[[nodiscard]] std::shared_ptr<ai::Provider> make_scripted_fake_provider(
    std::string provider_id = "fake");
[[nodiscard]] std::shared_ptr<ai::Models> make_scripted_fake_models();
[[nodiscard]] std::unique_ptr<ai::StreamingChatClient> make_scripted_fake_stream();

} // namespace cch::ai::providers
