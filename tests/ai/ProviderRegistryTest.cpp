#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../include/cch/ai/ProviderRegistry.hpp"
#include "../../include/cch/util/Error.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

class NullStreamingClient final : public ai::StreamingChatClient {
public:
    boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink) override {
        ai::AssistantMessage message = ai::assistant_text_message("null");
        message.model = request.model;
        message.provider = "null";
        co_return message;
    }
};

ai::ProviderFactoryResult make_null_client(const ai::ProviderFactoryContext&) {
    std::unique_ptr<ai::StreamingChatClient> client = std::make_unique<NullStreamingClient>();
    return client;
}

struct RunResult {
    util::Expected<ai::AssistantMessage> result;
    std::vector<ai::AssistantStreamEvent> events;
};

RunResult run_client(ai::StreamingChatClient& client, ai::StreamChatRequest request) {
    boost::asio::io_context io;
    std::optional<util::Expected<ai::AssistantMessage>> result;
    std::vector<ai::AssistantStreamEvent> events;

    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await client.stream(
                std::move(request),
                [&](const ai::AssistantStreamEvent& event) {
                    events.push_back(event);
                    return util::ExpectedVoid{};
                });
            co_return;
        },
        boost::asio::detached);

    io.run();
    REQUIRE(result.has_value());
    return RunResult{std::move(*result), std::move(events)};
}

std::string text_from_content(const std::vector<ai::Content>& content) {
    std::string text;
    for (const auto& block : content) {
        if (const auto* text_block = std::get_if<ai::TextContent>(&block)) {
            text += text_block->text;
        }
    }
    return text;
}

} // namespace

TEST_CASE("provider registry creates registered clients", "[ai][provider][registry]") {
    ai::ProviderRegistry registry;
    REQUIRE(registry.register_provider("null", make_null_client));

    ai::ProviderFactoryContext context;
    context.model = "model-from-context";
    auto client = registry.create("null", context);

    REQUIRE(client);
    ai::StreamChatRequest request;
    request.model = context.model;
    auto run = run_client(**client, std::move(request));
    REQUIRE(run.result);
    CHECK(run.result->provider == "null");
    CHECK(run.result->model == "model-from-context");
}

TEST_CASE("provider registry rejects duplicate and unknown providers", "[ai][provider][registry]") {
    ai::ProviderRegistry registry;
    REQUIRE(registry.register_provider("null", make_null_client));

    auto duplicate = registry.register_provider("null", make_null_client);
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().code == util::ErrorCode::Provider);
    CHECK(duplicate.error().message == "provider is already registered");

    auto missing = registry.create("missing", ai::ProviderFactoryContext{});
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == util::ErrorCode::Provider);
    CHECK(missing.error().message == "unknown provider");
    CHECK(missing.error().detail == "missing");
}

TEST_CASE("default provider registry includes fake and OpenAI-compatible providers", "[ai][provider][registry]") {
    auto registry = ai::make_default_provider_registry();
    REQUIRE(registry);

    CHECK(registry->contains("fake"));
    CHECK(registry->contains("openai-compatible"));

    ai::ProviderFactoryContext context;
    context.model = "fake-model";
    auto fake = registry->create("fake", context);
    REQUIRE(fake);

    ai::StreamChatRequest request;
    request.model = context.model;
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});
    auto run = run_client(**fake, std::move(request));

    REQUIRE(run.result);
    CHECK(run.result->provider == "fake");
    CHECK(run.result->api == "scripted-fake");
    CHECK(text_from_content(run.result->content) == "fake: hello");
    CHECK_FALSE(run.events.empty());

    auto openai = registry->create("openai-compatible", context);
    REQUIRE(openai);
}
