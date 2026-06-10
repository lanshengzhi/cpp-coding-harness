#pragma once

#include "../ChatClient.hpp"
#include "HttpTransport.hpp"

#include <chrono>
#include <memory>
#include <string>

namespace cch::ai::providers {

struct OpenAIConfig {
    std::string base_url{"https://api.openai.com"};
    std::string api_key;
    std::string api_key_env{"OPENAI_API_KEY"};
    std::string model{"gpt-4.1-mini"};
    std::string organization;
    std::string project;
    std::chrono::milliseconds timeout{30000};
};

class OpenAIChatClient : public ai::ChatClient {
public:
    OpenAIChatClient(std::shared_ptr<HttpTransport> transport, OpenAIConfig config);

    [[nodiscard]] util::Result<ai::ChatResponse> complete(const ai::ChatRequest& request) override;

private:
    [[nodiscard]] util::Result<std::string> resolve_api_key() const;
    [[nodiscard]] std::string completions_url() const;

    std::shared_ptr<HttpTransport> transport_;
    OpenAIConfig config_;
};

} // namespace cch::ai::providers
