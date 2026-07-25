#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../include/cch/ai/Content.hpp"
#include "../../include/cch/ai/ProviderRegistry.hpp"
#include "../../include/cch/util/Error.hpp"
#include "../support/UsageAssertions.hpp"

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
        message.model = request.model->id;
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

template <typename T>
std::vector<const T*> events_of(const std::vector<ai::AssistantStreamEvent>& events) {
    std::vector<const T*> matches;
    for (const auto& event : events) {
        if (const auto* value = std::get_if<T>(&event)) {
            matches.push_back(value);
        }
    }
    return matches;
}

} // namespace

TEST_CASE("provider registry creates registered clients", "[ai][provider][registry]") {
    ai::ProviderRegistry registry;
    REQUIRE(registry.register_provider("null", make_null_client));

    ai::ProviderFactoryContext context;
    context.model = ai::Model{"model-from-context"};
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
    context.model = ai::Model{"fake-model"};
    auto fake = registry->create("fake", context);
    REQUIRE(fake);

    ai::StreamChatRequest request;
    request.model = context.model;
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});
    auto run = run_client(**fake, std::move(request));

    REQUIRE(run.result);
    CHECK(run.result->provider == "fake");
    CHECK(run.result->api == "scripted-fake");
    CHECK(text_from_assistant_content(run.result->content) == "fake: hello");
    CHECK_FALSE(run.events.empty());

    auto openai = registry->create("openai-compatible", context);
    REQUIRE(openai);
}

TEST_CASE(
    "scripted fake provider emits and returns complete stable identity",
    "[ai][provider][registry][issue17][issue19]") {
    auto registry = ai::make_default_provider_registry();
    REQUIRE(registry);

    ai::ProviderFactoryContext context;
    context.model = ai::Model{"fake-model"};
    auto fake = registry->create("fake", context);
    REQUIRE(fake);

    ai::StreamChatRequest request;
    request.model = context.model;
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});
    auto run = run_client(**fake, std::move(request));

    REQUIRE(run.result);
    tests::check_zero_usage(run.result->usage);
    CHECK(run.result->api == "scripted-fake");
    CHECK(run.result->provider == "fake");
    CHECK(run.result->model == "fake-model");
    CHECK(run.result->timestamp > 0);

    const auto starts = events_of<ai::AssistantStartEvent>(run.events);
    REQUIRE(starts.size() == 1);
    tests::check_zero_usage(starts[0]->partial.usage);
    CHECK(starts[0]->partial.api == run.result->api);
    CHECK(starts[0]->partial.provider == run.result->provider);
    CHECK(starts[0]->partial.model == run.result->model);
    CHECK(starts[0]->partial.timestamp == run.result->timestamp);

    const auto done = events_of<ai::AssistantDoneEvent>(run.events);
    REQUIRE(done.size() == 1);
    tests::check_zero_usage(done[0]->message.usage);
    CHECK(done[0]->message.api == run.result->api);
    CHECK(done[0]->message.provider == run.result->provider);
    CHECK(done[0]->message.model == run.result->model);
    CHECK(done[0]->message.timestamp == run.result->timestamp);
}

TEST_CASE(
    "scripted fake provider rejects an empty requested model before events",
    "[ai][provider][registry][issue19]") {
    auto registry = ai::make_default_provider_registry();
    REQUIRE(registry);
    auto fake = registry->create("fake", ai::ProviderFactoryContext{});
    REQUIRE(fake);
    ai::StreamChatRequest request;

    auto run = run_client(**fake, std::move(request));

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::Validation);
    CHECK(run.events.empty());
}

TEST_CASE("fake provider emits read tool calls from prompts", "[ai][provider][registry]") {
    auto registry = ai::make_default_provider_registry();
    REQUIRE(registry);

    ai::ProviderFactoryContext context;
    context.model = ai::Model{"fake-model"};
    auto fake = registry->create("fake", context);
    REQUIRE(fake);

    ai::StreamChatRequest request;
    request.model = context.model;
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("read README.md")});
    auto run = run_client(**fake, std::move(request));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::ToolUse);
    REQUIRE(run.result->content.size() == 2);
    REQUIRE(std::holds_alternative<ai::ToolCallContent>(run.result->content[1]));
    const auto& call = std::get<ai::ToolCallContent>(run.result->content[1]);
    CHECK(call.id == "fake-read-1");
    CHECK(call.name == "read");
    CHECK(call.raw_arguments.find("README.md") != std::string::npos);
    REQUIRE(call.arguments);
    CHECK(call.arguments->at("path").get_string() == "README.md");
    CHECK_FALSE(events_of<ai::TextDeltaEvent>(run.events).empty());
}

TEST_CASE("fake provider emits bash tool calls from prompts", "[ai][provider][registry]") {
    auto registry = ai::make_default_provider_registry();
    REQUIRE(registry);

    ai::ProviderFactoryContext context;
    context.model = ai::Model{"fake-model"};
    auto fake = registry->create("fake", context);
    REQUIRE(fake);

    ai::StreamChatRequest request;
    request.model = context.model;
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("bash echo hi")});
    auto run = run_client(**fake, std::move(request));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::ToolUse);
    REQUIRE(run.result->content.size() == 2);
    REQUIRE(std::holds_alternative<ai::ToolCallContent>(run.result->content[1]));
    const auto& call = std::get<ai::ToolCallContent>(run.result->content[1]);
    CHECK(call.id == "fake-bash-1");
    CHECK(call.name == "bash");
    CHECK(call.raw_arguments.find("echo hi") != std::string::npos);
    REQUIRE(call.arguments);
    CHECK(call.arguments->at("command").get_string() == "echo hi");
    CHECK_FALSE(events_of<ai::TextDeltaEvent>(run.events).empty());
}

TEST_CASE("fake provider observes trailing tool results", "[ai][provider][registry]") {
    auto registry = ai::make_default_provider_registry();
    REQUIRE(registry);

    ai::ProviderFactoryContext context;
    context.model = ai::Model{"fake-model"};
    auto fake = registry->create("fake", context);
    REQUIRE(fake);

    ai::StreamChatRequest request;
    request.model = context.model;
    request.context.messages.push_back(ai::MessageVariant{ai::tool_result_message("fake-read-1", "read", "file contents")});
    auto run = run_client(**fake, std::move(request));

    REQUIRE(run.result);
    CHECK(ai::text_from_assistant_content(run.result->content) == "fake observed: file contents");
    CHECK_FALSE(events_of<ai::TextDeltaEvent>(run.events).empty());
}
