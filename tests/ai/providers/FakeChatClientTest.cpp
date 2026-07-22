#include "../../../third_party/catch2/catch_test_macros.hpp"

#include "../../../include/cch/ai/ChatClient.hpp"
#include "../../../include/cch/ai/Content.hpp"
#include "../../../include/cch/ai/Message.hpp"
#include "../../../include/cch/util/Error.hpp"
#include "../../../src/ai/providers/FakeChatClient.hpp"
#include "../../support/UsageAssertions.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

struct RunResult {
    util::Expected<ai::AssistantMessage> result;
    std::vector<ai::AssistantStreamEvent> events;
};

RunResult run_fake(
    ai::StreamChatRequest request,
    std::optional<std::size_t> fail_at_event = std::nullopt) {
    boost::asio::io_context io;
    auto client = ai::providers::make_scripted_fake_chat_client();
    std::optional<util::Expected<ai::AssistantMessage>> result;
    std::vector<ai::AssistantStreamEvent> events;

    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await client->stream(
                request,
                [&](const ai::AssistantStreamEvent& event) -> util::ExpectedVoid {
                    events.push_back(event);
                    if (fail_at_event && events.size() - 1 == *fail_at_event) {
                        return std::unexpected(util::make_error(
                            util::ErrorCode::Unknown,
                            "fake sink rejected event",
                            std::to_string(*fail_at_event)));
                    }
                    return {};
                });
            co_return;
        },
        boost::asio::detached);

    io.run();
    REQUIRE(result.has_value());
    return RunResult{std::move(*result), std::move(events)};
}

ai::StreamChatRequest request_with(ai::MessageVariant message) {
    ai::StreamChatRequest request;
    request.model = "fake-model";
    request.context.messages.push_back(std::move(message));
    return request;
}

template <typename Event>
const Event& require_event(const std::vector<ai::AssistantStreamEvent>& events, std::size_t index) {
    REQUIRE(index < events.size());
    const auto* event = std::get_if<Event>(&events[index]);
    REQUIRE(event != nullptr);
    return *event;
}

void check_same_metadata(
    const ai::AssistantMessage& actual,
    const ai::AssistantMessage& expected) {
    CHECK(actual.api == expected.api);
    CHECK(actual.provider == expected.provider);
    CHECK(actual.model == expected.model);
    CHECK(actual.response_model == expected.response_model);
    CHECK(actual.response_id == expected.response_id);
    tests::check_zero_usage(actual.usage);
    CHECK(actual.stop_reason == expected.stop_reason);
    CHECK(actual.error_message == expected.error_message);
    CHECK(actual.diagnostics.has_value() == expected.diagnostics.has_value());
    CHECK(actual.timestamp == expected.timestamp);
}

const ai::TextContent& require_text(const ai::AssistantMessage& message, std::size_t index) {
    REQUIRE(index < message.content.size());
    const auto* text = std::get_if<ai::TextContent>(&message.content[index]);
    REQUIRE(text != nullptr);
    return *text;
}

const ai::ToolCallContent& require_tool_call(
    const ai::AssistantMessage& message,
    std::size_t index) {
    REQUIRE(index < message.content.size());
    const auto* call = std::get_if<ai::ToolCallContent>(&message.content[index]);
    REQUIRE(call != nullptr);
    return *call;
}

void check_nonempty_text_block_lifecycle(
    const std::vector<ai::AssistantStreamEvent>& events,
    std::size_t first_event_index,
    const ai::AssistantMessage& final_message,
    std::size_t content_index,
    const std::string& expected_text) {
    const auto& text_start =
        require_event<ai::TextStartEvent>(events, first_event_index);
    CHECK(text_start.content_index == content_index);
    check_same_metadata(text_start.partial, final_message);
    REQUIRE(text_start.partial.content.size() == content_index + 1);
    CHECK(require_text(text_start.partial, content_index).text.empty());

    const auto& text_delta =
        require_event<ai::TextDeltaEvent>(events, first_event_index + 1);
    CHECK(text_delta.content_index == content_index);
    CHECK(text_delta.delta == expected_text);
    check_same_metadata(text_delta.partial, final_message);
    CHECK(require_text(text_delta.partial, content_index).text == expected_text);

    const auto& text_end =
        require_event<ai::TextEndEvent>(events, first_event_index + 2);
    CHECK(text_end.content_index == content_index);
    CHECK(text_end.content == expected_text);
    check_same_metadata(text_end.partial, final_message);
    CHECK(require_text(text_end.partial, content_index).text == expected_text);
}

void check_text_lifecycle(
    const RunResult& run,
    const std::string& expected_text) {
    REQUIRE(run.result.has_value());
    const auto& final_message = *run.result;
    REQUIRE(final_message.content.size() == 1);
    CHECK(require_text(final_message, 0).text == expected_text);
    REQUIRE(run.events.size() == 5);

    const auto& assistant_start = require_event<ai::AssistantStartEvent>(run.events, 0);
    check_same_metadata(assistant_start.partial, final_message);
    CHECK(assistant_start.partial.content.empty());

    check_nonempty_text_block_lifecycle(
        run.events,
        1,
        final_message,
        0,
        expected_text);

    const auto& done = require_event<ai::AssistantDoneEvent>(run.events, 4);
    CHECK(done.reason == final_message.stop_reason);
    check_same_metadata(done.message, final_message);
    REQUIRE(done.message.content.size() == 1);
    CHECK(require_text(done.message, 0).text == expected_text);
}

void check_tool_lifecycle(
    const RunResult& run,
    const std::string& expected_text,
    const std::string& expected_id,
    const std::string& expected_name,
    const std::string& expected_argument_name,
    const std::string& expected_argument_value) {
    REQUIRE(run.result.has_value());
    const auto& final_message = *run.result;
    CHECK(final_message.stop_reason == ai::AssistantStopReason::ToolUse);
    REQUIRE(final_message.content.size() == 2);
    CHECK(require_text(final_message, 0).text == expected_text);
    const auto& final_call = require_tool_call(final_message, 1);
    CHECK(final_call.id == expected_id);
    CHECK(final_call.name == expected_name);
    REQUIRE(final_call.arguments.has_value());
    CHECK(final_call.arguments->at(expected_argument_name).get_string() == expected_argument_value);
    REQUIRE_FALSE(final_call.raw_arguments.empty());
    REQUIRE(run.events.size() == 8);

    const auto& assistant_start = require_event<ai::AssistantStartEvent>(run.events, 0);
    check_same_metadata(assistant_start.partial, final_message);
    CHECK(assistant_start.partial.content.empty());

    check_nonempty_text_block_lifecycle(
        run.events,
        1,
        final_message,
        0,
        expected_text);

    const auto& call_start = require_event<ai::ToolCallStartEvent>(run.events, 4);
    CHECK(call_start.content_index == 1);
    check_same_metadata(call_start.partial, final_message);
    REQUIRE(call_start.partial.content.size() == 2);
    CHECK(require_text(call_start.partial, 0).text == expected_text);
    const auto& empty_call = require_tool_call(call_start.partial, 1);
    CHECK(empty_call.id.empty());
    CHECK(empty_call.name.empty());
    CHECK_FALSE(empty_call.arguments.has_value());
    CHECK(empty_call.raw_arguments.empty());

    const auto& call_delta = require_event<ai::ToolCallDeltaEvent>(run.events, 5);
    CHECK(call_delta.content_index == 1);
    CHECK(call_delta.delta == final_call.raw_arguments);
    check_same_metadata(call_delta.partial, final_message);
    const auto& partial_call = require_tool_call(call_delta.partial, 1);
    CHECK(partial_call.id == expected_id);
    CHECK(partial_call.name == expected_name);
    CHECK_FALSE(partial_call.arguments.has_value());
    CHECK(partial_call.raw_arguments == call_delta.delta);

    const auto& call_end = require_event<ai::ToolCallEndEvent>(run.events, 6);
    CHECK(call_end.content_index == 1);
    CHECK(call_end.tool_call.id == final_call.id);
    CHECK(call_end.tool_call.name == final_call.name);
    CHECK(call_end.tool_call.raw_arguments == final_call.raw_arguments);
    REQUIRE(call_end.tool_call.arguments.has_value());
    CHECK(call_end.tool_call.arguments->at(expected_argument_name).get_string() == expected_argument_value);
    const auto& ended_call = require_tool_call(call_end.partial, 1);
    CHECK(ended_call.id == final_call.id);
    CHECK(ended_call.name == final_call.name);
    CHECK(ended_call.raw_arguments == final_call.raw_arguments);
    REQUIRE(ended_call.arguments.has_value());
    CHECK(ended_call.arguments->at(expected_argument_name).get_string() == expected_argument_value);

    const auto& done = require_event<ai::AssistantDoneEvent>(run.events, 7);
    CHECK(done.reason == final_message.stop_reason);
    check_same_metadata(done.message, final_message);
    REQUIRE(done.message.content.size() == 2);
    CHECK(require_text(done.message, 0).text == expected_text);
    const auto& done_call = require_tool_call(done.message, 1);
    CHECK(done_call.id == final_call.id);
    CHECK(done_call.name == final_call.name);
    CHECK(done_call.raw_arguments == final_call.raw_arguments);
    REQUIRE(done_call.arguments.has_value());
    CHECK(done_call.arguments->at(expected_argument_name).get_string() == expected_argument_value);
}

} // namespace

TEST_CASE(
    "scripted fake emits complete text lifecycles for every text-only response path",
    "[ai][provider][fake][issue23]") {
    auto prompt_run = run_fake(request_with(ai::user_text_message("hello")));
    check_text_lifecycle(prompt_run, "fake: hello");

    auto tool_result_run = run_fake(request_with(
        ai::tool_result_message("fake-read-1", "read", "file contents")));
    check_text_lifecycle(tool_result_run, "fake observed: file contents");
}

TEST_CASE(
    "scripted fake emits complete ordered read and bash tool lifecycles",
    "[ai][provider][fake][issue23]") {
    auto read_run = run_fake(request_with(ai::user_text_message("read README.md")));
    check_tool_lifecycle(
        read_run,
        "reading README.md",
        "fake-read-1",
        "read",
        "path",
        "README.md");

    auto bash_run = run_fake(request_with(ai::user_text_message("bash echo hi")));
    check_tool_lifecycle(
        bash_run,
        "running bash",
        "fake-bash-1",
        "bash",
        "command",
        "echo hi");
}

TEST_CASE(
    "scripted fake rejects static requests without starting a lifecycle",
    "[ai][provider][fake][issue23]") {
    ai::StreamChatRequest request;
    auto run = run_fake(std::move(request));

    REQUIRE_FALSE(run.result.has_value());
    CHECK(run.result.error().code == util::ErrorCode::Validation);
    CHECK(run.events.empty());
}

TEST_CASE(
    "scripted fake stops at and propagates every sink failure",
    "[ai][provider][fake][issue23]") {
    constexpr std::size_t expected_event_count = 8;
    for (std::size_t fail_at = 0; fail_at < expected_event_count; ++fail_at) {
        auto run = run_fake(
            request_with(ai::user_text_message("read README.md")),
            fail_at);

        REQUIRE_FALSE(run.result.has_value());
        CHECK(run.result.error().code == util::ErrorCode::Unknown);
        CHECK(run.result.error().message == "fake sink rejected event");
        CHECK(run.result.error().detail == std::to_string(fail_at));
        CHECK(run.events.size() == fail_at + 1);
    }
}
