#include "ai/api/MessageConversion.hpp"
#include "support/ModelFixture.hpp"
#include "support/Json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::vector<std::string> payload_string_leaves(const support::JsonValue& payload) {
    std::vector<std::string> texts;
    const auto& messages = payload.at("messages").get_array();
    const auto collect = [&texts](const auto& self, const support::JsonValue& value) -> void {
        if (value.holds<support::JsonValue::array_t>()) {
            for (const auto& item : value.get<support::JsonValue::array_t>()) {
                self(self, item);
            }
        } else if (value.holds<support::JsonValue::object_t>()) {
            for (const auto& [_, item] : value.get<support::JsonValue::object_t>()) {
                self(self, item);
            }
        } else if (value.holds<std::string>()) {
            texts.push_back(value.get<std::string>());
        }
    };
    for (const auto& message : messages) {
        collect(collect, message);
    }
    return texts;
}

[[nodiscard]] bool any_leaf_contains(const std::vector<std::string>& texts, std::string_view needle) {
    return std::ranges::any_of(
            texts, [needle](const std::string& text) { return text.find(needle) != std::string::npos; });
}

[[nodiscard]] ai::BashExecutionMessage user_bash(
        std::string command, std::string output, bool exclude_from_context, ai::TimestampMs timestamp) {
    ai::BashExecutionMessage message;
    message.command = std::move(command);
    message.output = std::move(output);
    message.exclude_from_context = exclude_from_context;
    message.timestamp = timestamp;
    return message;
}

[[nodiscard]] ai::Model completions_model() { return tests::make_model("text-only", "deepseek", "openai-completions"); }

} // namespace

TEST_CASE("Completions conversion sanitizes text and fills orphaned tool results",
        "[ai][conversion][issue339][compat-pi]") {
    const auto model = completions_model();
    ai::AiContext context;
    context.messages.push_back(ai::UserMessage{
            .content =
                    std::vector<ai::Content>{
                            ai::text_content(std::string{"bad"} + "\xed\xa0\x80"),
                            ai::image_content("YWJj", "image/png"),
                    },
            .timestamp = 1,
    });
    ai::AssistantMessage assistant;
    assistant.api = model.api;
    assistant.provider = model.provider;
    assistant.model = model.id;
    assistant.stop_reason = ai::AssistantStopReason::ToolUse;
    assistant.content.push_back(ai::tool_call_content("orphan!", "lookup", "{}", support::JsonValue::object_t{}));
    context.messages.push_back(std::move(assistant));
    const ai::api::PreparedStreamOptions options;

    const auto payload = ai::api::build_openai_completions_payload(model, context, options);

    REQUIRE(payload);
    const auto texts = payload_string_leaves(*payload);
    CHECK(any_leaf_contains(texts, std::string{"bad"} + "\xef\xbf\xbd"));
    CHECK(any_leaf_contains(texts, "(image omitted: model does not support images)"));
    CHECK(any_leaf_contains(texts, "No result provided"));
    CHECK(any_leaf_contains(texts, "orphan"));
}

TEST_CASE("String-content user messages pass the image downgrade untouched", "[ai][conversion][issue365][compat-pi]") {
    const auto model = completions_model();
    ai::AiContext context;
    context.messages.push_back(ai::UserMessage{
            .content = std::string{"plain string content"},
            .timestamp = 1,
    });
    const ai::api::PreparedStreamOptions options;

    const auto payload = ai::api::build_openai_completions_payload(model, context, options);

    REQUIRE(payload);
    const auto& messages = payload->at("messages").get_array();
    REQUIRE(messages.size() == 1);
    CHECK(messages.front().at("content").get_string() == "plain string content");
}

TEST_CASE("Synthesized user messages arrive as Completions content", "[ai][conversion][issue365][compat-pi]") {
    const auto model = completions_model();
    ai::AiContext context;
    context.messages.push_back(user_bash("echo hello", "hello\n", false, 1));
    const ai::api::PreparedStreamOptions options;

    const auto payload = ai::api::build_openai_completions_payload(model, context, options);

    REQUIRE(payload);
    const auto& messages = payload->at("messages").get_array();
    REQUIRE(messages.size() == 1);
    CHECK(messages.front().at("role").get_string() == "user");
    CHECK(any_leaf_contains(payload_string_leaves(*payload), "Ran `echo hello`"));
}

TEST_CASE("Excluded bash output never reaches the Completions payload", "[ai][conversion][issue668][compat-pi]") {
    const auto model = completions_model();
    ai::AiContext context;
    context.messages.push_back(user_bash("echo included", "included output\n", false, 1));
    context.messages.push_back(ai::user_text_message("later prompt", 2));
    context.messages.push_back(user_bash("echo excluded", "excluded output\n", true, 3));
    const ai::api::PreparedStreamOptions options;

    const auto payload = ai::api::build_openai_completions_payload(model, context, options);

    REQUIRE(payload);
    const auto texts = payload_string_leaves(*payload);
    CHECK_FALSE(any_leaf_contains(texts, "excluded output"));
    CHECK(any_leaf_contains(texts, "included output"));
}

TEST_CASE("An all-excluded context converts to an empty Completions payload", "[ai][conversion][issue668][compat-pi]") {
    const auto model = completions_model();
    ai::AiContext context;
    context.messages.push_back(user_bash("echo excluded", "excluded output\n", true, 1));
    const ai::api::PreparedStreamOptions options;

    const auto payload = ai::api::build_openai_completions_payload(model, context, options);

    REQUIRE(payload);
    CHECK(payload->at("messages").get_array().empty());
}

TEST_CASE("An excluded message between a tool call and result does not orphan the call",
        "[ai][conversion][issue668][compat-pi]") {
    const auto model = completions_model();
    ai::AssistantMessage assistant;
    assistant.api = model.api;
    assistant.provider = model.provider;
    assistant.model = model.id;
    assistant.stop_reason = ai::AssistantStopReason::ToolUse;
    assistant.timestamp = 1;
    assistant.content.push_back(ai::tool_call_content("call-1", "bash", R"({"command":"echo x"})"));
    ai::AiContext context;
    context.messages.push_back(std::move(assistant));
    context.messages.push_back(user_bash("echo excluded", "excluded output\n", true, 2));
    context.messages.push_back(ai::tool_result_message("call-1", "bash", "real tool output", false, 3));
    const ai::api::PreparedStreamOptions options;

    const auto payload = ai::api::build_openai_completions_payload(model, context, options);

    REQUIRE(payload);
    const auto texts = payload_string_leaves(*payload);
    CHECK_FALSE(any_leaf_contains(texts, "No result provided"));
    CHECK_FALSE(any_leaf_contains(texts, "excluded output"));
    CHECK(any_leaf_contains(texts, "real tool output"));
}
