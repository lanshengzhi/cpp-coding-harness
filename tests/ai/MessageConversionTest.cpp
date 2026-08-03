#include "ai/api/MessageConversion.hpp"
#include "support/ModelFixture.hpp"
#include "support/PiFixture.hpp"
#include "util/Json.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <string>

using namespace cch;

namespace {

void check_payload_fixture(
    const util::JsonValue& actual,
    std::string_view relative_path) {
    const auto expected = tests::read_pi_fixture(relative_path);
    REQUIRE(expected);
    const auto actual_json = util::write_json(actual);
    const auto expected_json = util::write_json(*expected);
    REQUIRE(actual_json);
    REQUIRE(expected_json);
    CHECK(*actual_json == *expected_json);
}

ai::AiContext responses_context() {
    ai::AiContext context;
    context.system_prompt = "system";
    context.messages.push_back(ai::UserMessage{
        .content = {ai::text_content("hi"), ai::image_content("YWJj", "image/png")},
        .timestamp = 1,
    });
    ai::AssistantMessage assistant;
    assistant.api = "openai-responses";
    assistant.provider = "deepseek";
    assistant.model = "deepseek-v4-flash";
    assistant.stop_reason = ai::AssistantStopReason::ToolUse;
    assistant.timestamp = 2;
    assistant.content.push_back(ai::ThinkingContent{
        .thinking = "",
        .thinking_signature =
            "{\"encrypted_content\":\"dummy-encrypted\",\"id\":\"rs_1\",\"summary\":[],\"type\":\"reasoning\"}",
    });
    assistant.content.push_back(ai::TextContent{
        .text = "answer",
        .text_signature = "{\"v\":1,\"id\":\"msg_1\",\"phase\":\"final_answer\"}",
    });
    context.messages.push_back(std::move(assistant));
    context.messages.push_back(ai::user_text_message("next", 3));

    ai::AssistantMessage foreign_assistant;
    foreign_assistant.api = "openai-codex-responses";
    foreign_assistant.provider = "openai-codex";
    foreign_assistant.model = "gpt-5.5";
    foreign_assistant.stop_reason = ai::AssistantStopReason::ToolUse;
    foreign_assistant.timestamp = 4;
    foreign_assistant.content.push_back(ai::ToolCallContent{
        .id = "call bad!|item bad!",
        .name = "lookup",
        .arguments = util::JsonValue::object_t{{"q", "x"}},
        .raw_arguments = "{\"q\":\"x\"}",
        .thought_signature = std::nullopt,
        .arguments_valid = true,
        .argument_error = std::nullopt,
    });
    context.messages.push_back(std::move(foreign_assistant));
    context.messages.push_back(ai::ToolResultMessage{
        .tool_call_id = "call bad!|item bad!",
        .tool_name = "lookup",
        .content = {ai::text_content("ok")},
        .details = std::nullopt,
        .is_error = true,
        .timestamp = 5,
    });
    context.tools.push_back(ai::Tool{
        .name = "lookup",
        .description = "Look up a value",
        .parameters = util::JsonValue::object_t{
            {"properties", util::JsonValue::object_t{{"q", util::JsonValue::object_t{{"type", "string"}}}}},
            {"required", util::JsonValue::array_t{"q"}},
            {"type", "object"},
        },
    });
    return context;
}

} // namespace

TEST_CASE("Responses conversion matches the frozen DeepSeek payload golden", "[ai][conversion][issue339]") {
    auto model = tests::make_model(
        "deepseek-v4-flash", "deepseek", "openai-responses");
    model.reasoning = true;
    model.input = {ai::ModelInput::Text, ai::ModelInput::Image};
    model.thinking_level_map = ai::ThinkingLevelMap{{ai::ModelThinkingLevel::High, "max"}};
    ai::ProviderStreamOptions options;
    options.temperature = 0.2;
    options.max_tokens = 123;
    options.reasoning = ai::ModelThinkingLevel::High;
    options.session_id = "session-1";
    options.cache_retention = ai::CacheRetention::Long;

    const auto payload = ai::api::build_adapter_payload(
        ai::api::AdapterKind::OpenAIResponses,
        model,
        responses_context(),
        options);

    REQUIRE(payload);
    check_payload_fixture(*payload, "conversion/openai-responses-deepseek.json");
}

TEST_CASE(
    "Responses conversion sanitizes malformed text and fills orphaned tool results",
    "[ai][conversion][issue339]") {
    auto model = tests::make_model("text-only", "deepseek", "openai-responses");
    ai::AiContext context;
    context.messages.push_back(ai::UserMessage{
        .content = {
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
    assistant.content.push_back(ai::tool_call_content("orphan!", "lookup", "{}", util::JsonValue::object_t{}));
    context.messages.push_back(std::move(assistant));
    ai::ProviderStreamOptions options;

    const auto payload = ai::api::build_adapter_payload(
        ai::api::AdapterKind::OpenAIResponses,
        model,
        context,
        options);

    REQUIRE(payload);
    const auto serialized = util::write_json(*payload);
    REQUIRE(serialized);
    const auto sanitized = std::string{"bad"} + "\xef\xbf\xbd";
    CHECK(serialized->find(sanitized) != std::string::npos);
    CHECK(serialized->find(std::string{"\xef\xbf\xbd"} + "\xef\xbf\xbd") ==
          std::string::npos);
    CHECK(serialized->find("(image omitted: model does not support images)") != std::string::npos);
    CHECK(serialized->find("No result provided") != std::string::npos);
    CHECK(serialized->find("orphan") != std::string::npos);
}

TEST_CASE(
    "Omitted reasoning uses each scoped adapter default without a summary",
    "[ai][conversion][issue339]") {
    auto model = tests::make_model("deepseek-v4-flash", "deepseek", "openai-responses");
    model.reasoning = true;
    ai::ProviderStreamOptions options;
    const ai::AiContext context;

    const auto responses = ai::api::build_adapter_payload(
        ai::api::AdapterKind::OpenAIResponses,
        model,
        context,
        options);
    REQUIRE(responses);
    const auto& reasoning = responses->at("reasoning").get_object();
    CHECK(reasoning.at("effort").get_string() == "none");
    CHECK(reasoning.find("summary") == reasoning.end());

    model.id = "gpt-5.5";
    model.provider = "openai-codex";
    model.api = "openai-codex-responses";
    const auto codex = ai::api::build_adapter_payload(
        ai::api::AdapterKind::OpenAICodexResponses,
        model,
        context,
        options);
    REQUIRE(codex);
    CHECK(codex->get_object().find("reasoning") == codex->get_object().end());
}

TEST_CASE(
    "Responses replay drops empty signatures and clamps prompt cache keys",
    "[ai][conversion][issue339]") {
    auto model = tests::make_model("deepseek-v4-flash", "deepseek", "openai-responses");
    ai::AiContext context;
    ai::AssistantMessage assistant;
    assistant.api = model.api;
    assistant.provider = model.provider;
    assistant.model = model.id;
    assistant.stop_reason = ai::AssistantStopReason::Stop;
    assistant.content = {
        ai::ThinkingContent{.thinking = "", .thinking_signature = ""},
        ai::TextContent{
            .text = "answer",
            .text_signature = "{\"v\":1,\"id\":\"\"}",
        },
    };
    context.messages.push_back(std::move(assistant));
    ai::ProviderStreamOptions options;
    options.session_id = std::string(65, 's');

    const auto payload = ai::api::build_adapter_payload(
        ai::api::AdapterKind::OpenAIResponses,
        model,
        context,
        options);

    REQUIRE(payload);
    CHECK(payload->at("prompt_cache_key").get_string() == std::string(64, 's'));
    const auto serialized = util::write_json(*payload);
    REQUIRE(serialized);
    CHECK(serialized->find("msg_pi_0") != std::string::npos);
    CHECK(serialized->find("Invalid Responses thinking signature") == std::string::npos);
}

TEST_CASE("Codex conversion matches the frozen Responses payload golden", "[ai][conversion][issue339]") {
    auto model = tests::make_model(
        "gpt-5.5", "openai-codex", "openai-codex-responses");
    model.reasoning = true;
    model.thinking_level_map = ai::ThinkingLevelMap{
        {ai::ModelThinkingLevel::XHigh, "xhigh"},
    };
    ai::AiContext context;
    context.messages.push_back(ai::user_text_message("hello", 1));

    ai::AssistantMessage foreign_assistant;
    foreign_assistant.api = "openai-responses";
    foreign_assistant.provider = "deepseek";
    foreign_assistant.model = "deepseek-v4-flash";
    foreign_assistant.stop_reason = ai::AssistantStopReason::ToolUse;
    foreign_assistant.timestamp = 2;
    foreign_assistant.content.push_back(ai::tool_call_content(
        "call bad!|foreign/item|ignored",
        "lookup",
        "{\"q\":\"x\"}",
        util::JsonValue::object_t{{"q", "x"}}));
    context.messages.push_back(std::move(foreign_assistant));
    context.messages.push_back(ai::ToolResultMessage{
        .tool_call_id = "call bad!|foreign/item|ignored",
        .tool_name = "lookup",
        .content = {ai::text_content("foreign result")},
        .details = std::nullopt,
        .is_error = false,
        .timestamp = 3,
    });
    context.messages.push_back(ai::user_text_message("next", 4));

    ai::AssistantMessage different_model_assistant;
    different_model_assistant.api = model.api;
    different_model_assistant.provider = model.provider;
    different_model_assistant.model = "gpt-5.4";
    different_model_assistant.stop_reason = ai::AssistantStopReason::ToolUse;
    different_model_assistant.timestamp = 5;
    different_model_assistant.content.push_back(ai::tool_call_content(
        "other call|item bad!",
        "lookup",
        "{}",
        util::JsonValue::object_t{}));
    context.messages.push_back(std::move(different_model_assistant));
    context.messages.push_back(ai::ToolResultMessage{
        .tool_call_id = "other call|item bad!",
        .tool_name = "lookup",
        .content = {ai::text_content("different-model result")},
        .details = std::nullopt,
        .is_error = false,
        .timestamp = 6,
    });
    context.tools = responses_context().tools;
    ai::ProviderStreamOptions options;
    options.reasoning = ai::ModelThinkingLevel::XHigh;
    options.session_id = "codex-session";
    options.cache_retention = ai::CacheRetention::Short;

    const auto payload = ai::api::build_adapter_payload(
        ai::api::AdapterKind::OpenAICodexResponses,
        model,
        context,
        options);

    REQUIRE(payload);
    check_payload_fixture(*payload, "conversion/openai-codex-responses.json");
}

TEST_CASE("Anthropic conversion matches the frozen Kimi payload golden", "[ai][conversion][issue339]") {
    auto model = tests::make_model(
        "kimi-for-coding", "kimi-coding", "anthropic-messages");
    model.reasoning = true;
    model.input = {ai::ModelInput::Text, ai::ModelInput::Image};
    model.compat = ai::AnthropicMessagesCompat{
        .force_adaptive_thinking = true,
        .allow_empty_signature = true,
    };
    ai::AiContext context;
    context.system_prompt = "system";
    context.messages.push_back(ai::UserMessage{
        .content = {ai::image_content("YWJj", "image/png")},
        .timestamp = 1,
    });
    ai::AssistantMessage assistant;
    assistant.api = model.api;
    assistant.provider = model.provider;
    assistant.model = model.id;
    assistant.stop_reason = ai::AssistantStopReason::ToolUse;
    assistant.timestamp = 2;
    assistant.content = {
        ai::ThinkingContent{.thinking = "thought", .thinking_signature = ""},
        ai::ThinkingContent{
            .thinking = "[Reasoning redacted]",
            .thinking_signature = "dummy-redacted",
            .redacted = true,
        },
        ai::TextContent{.text = "answer", .text_signature = std::nullopt},
    };
    context.messages.push_back(std::move(assistant));
    context.messages.push_back(ai::user_text_message("next", 3));

    ai::AssistantMessage foreign_assistant;
    foreign_assistant.api = "openai-responses";
    foreign_assistant.provider = "deepseek";
    foreign_assistant.model = "deepseek-v4-flash";
    foreign_assistant.stop_reason = ai::AssistantStopReason::ToolUse;
    foreign_assistant.timestamp = 4;
    foreign_assistant.content.push_back(ai::ToolCallContent{
        .id = "bad id!",
        .name = "lookup",
        .arguments = util::JsonValue::object_t{{"q", "x"}},
        .raw_arguments = "{\"q\":\"x\"}",
        .thought_signature = std::nullopt,
        .arguments_valid = true,
        .argument_error = std::nullopt,
    });
    context.messages.push_back(std::move(foreign_assistant));
    context.messages.push_back(ai::ToolResultMessage{
        .tool_call_id = "bad id!",
        .tool_name = "lookup",
        .content = {ai::text_content("failed")},
        .details = std::nullopt,
        .is_error = true,
        .timestamp = 5,
    });
    context.messages.push_back(ai::ToolResultMessage{
        .tool_call_id = "second",
        .tool_name = "lookup",
        .content = {ai::image_content("ZGVm", "image/png")},
        .details = std::nullopt,
        .is_error = false,
        .timestamp = 6,
    });
    context.tools = responses_context().tools;
    ai::ProviderStreamOptions options;
    options.temperature = 0.5;
    options.max_tokens = 256;
    options.reasoning = ai::ModelThinkingLevel::High;
    options.cache_retention = ai::CacheRetention::Short;

    const auto payload = ai::api::build_adapter_payload(
        ai::api::AdapterKind::AnthropicMessages,
        model,
        context,
        options);

    REQUIRE(payload);
    check_payload_fixture(*payload, "conversion/anthropic-messages-kimi.json");
}

TEST_CASE(
    "Anthropic history caching does not mark an earlier user turn",
    "[ai][conversion][issue339]") {
    auto model = tests::make_model("kimi-for-coding", "kimi-coding", "anthropic-messages");
    ai::AiContext context;
    context.messages.push_back(ai::user_text_message("question", 1));
    ai::AssistantMessage assistant;
    assistant.api = model.api;
    assistant.provider = model.provider;
    assistant.model = model.id;
    assistant.stop_reason = ai::AssistantStopReason::Stop;
    assistant.content.push_back(ai::text_content("answer"));
    context.messages.push_back(std::move(assistant));
    ai::ProviderStreamOptions options;
    options.cache_retention = ai::CacheRetention::Short;

    const auto payload = ai::api::build_adapter_payload(
        ai::api::AdapterKind::AnthropicMessages,
        model,
        context,
        options);

    REQUIRE(payload);
    const auto serialized = util::write_json(*payload);
    REQUIRE(serialized);
    CHECK(serialized->find("cache_control") == std::string::npos);
}
