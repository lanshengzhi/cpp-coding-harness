#include "../../third_party/catch2/catch_test_macros.hpp"

#include "ComplexToolSchemaFixture.hpp"
#include "ai/glaze/AiJson.hpp"
#include "../../include/cch/util/Error.hpp"
#include "util/Json.hpp"

#include <string>
#include <variant>

using namespace cch;

TEST_CASE("tool-result message round-trips linkage details and error state", "[ai][u2][glaze]") {
    auto details = util::read_json<util::JsonValue>(R"({"exitCode":2,"stderr":"denied"})");
    REQUIRE(details);

    ai::ToolResultMessage result;
    result.tool_call_id = "call-1";
    result.tool_name = "read_file";
    result.content.emplace_back(ai::TextContent{"could not read", std::nullopt});
    result.content.emplace_back(ai::ImageContent{"ZmFrZQ==", "image/png"});
    result.details = *details;
    result.is_error = true;
    result.timestamp = 1718000000456;

    auto json = ai::glaze::write_message_json(ai::MessageVariant{result});
    REQUIRE(json);
    CHECK(json->find(R"("role":"toolResult")") != std::string::npos);
    CHECK(json->find(R"("toolCallId":"call-1")") != std::string::npos);
    CHECK(json->find(R"("isError":true)") != std::string::npos);

    auto parsed = ai::glaze::read_message_json(*json);
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(*parsed));
    const auto& round_trip = std::get<ai::ToolResultMessage>(*parsed);
    CHECK(round_trip.tool_call_id == "call-1");
    CHECK(round_trip.tool_name == "read_file");
    CHECK(round_trip.is_error);
    CHECK(round_trip.timestamp == 1718000000456);
    REQUIRE(round_trip.content.size() == 2);
    REQUIRE(std::holds_alternative<ai::TextContent>(round_trip.content[0]));
    CHECK(std::get<ai::TextContent>(round_trip.content[0]).text == "could not read");
    REQUIRE(std::holds_alternative<ai::ImageContent>(round_trip.content[1]));
    CHECK(std::get<ai::ImageContent>(round_trip.content[1]).mime_type == "image/png");
    REQUIRE(round_trip.details);
    const auto& detail_object = round_trip.details->get<util::JsonValue::object_t>();
    CHECK(static_cast<int>(detail_object.at("exitCode").get<double>()) == 2);
    CHECK(detail_object.at("stderr").get_string() == "denied");
}

TEST_CASE("assistant content round-trips text, thinking, and tool-call variants", "[ai][u2][glaze]") {
    ai::AssistantMessage msg;
    msg.content.emplace_back(ai::TextContent{"hello", std::nullopt});
    msg.content.emplace_back(ai::ThinkingContent{"reasoning", std::nullopt, false});
    msg.content.emplace_back(ai::ToolCallContent{"call-1", "bash", std::nullopt, R"({"cmd":"ls"})", std::nullopt, true, std::nullopt});
    msg.api = "openai-completions";
    msg.provider = "openai";
    msg.model = "gpt-test";
    msg.stop_reason = ai::AssistantStopReason::ToolUse;
    msg.timestamp = 1718000000123;

    auto json = ai::glaze::write_message_json(ai::MessageVariant{msg});
    REQUIRE(json);
    CHECK(json->find(R"("type":"text")") != std::string::npos);
    CHECK(json->find(R"("type":"thinking")") != std::string::npos);
    CHECK(json->find(R"("type":"toolCall")") != std::string::npos);

    auto parsed = ai::glaze::read_message_json(*json);
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(*parsed));
    const auto& round_trip = std::get<ai::AssistantMessage>(*parsed);
    REQUIRE(round_trip.content.size() == 3);
    REQUIRE(std::holds_alternative<ai::TextContent>(round_trip.content[0]));
    REQUIRE(std::holds_alternative<ai::ThinkingContent>(round_trip.content[1]));
    REQUIRE(std::holds_alternative<ai::ToolCallContent>(round_trip.content[2]));
}

TEST_CASE("assistant message round-trips diagnostics and cacheWrite1h", "[ai][u2][glaze]") {
    ai::AssistantMessage msg;
    msg.content.emplace_back(ai::TextContent{"test", std::nullopt});
    msg.api = "openai-completions";
    msg.provider = "openai";
    msg.model = "gpt-test";
    msg.stop_reason = ai::AssistantStopReason::Stop;
    msg.timestamp = 1718000000123;

    // Add diagnostics
    ai::DiagnosticErrorInfo err_info;
    err_info.name = "NetworkError";
    err_info.message = "connection refused";
    err_info.code = "ECONNREFUSED";
    ai::DiagnosticEntry diag;
    diag.type = "provider_error";
    diag.timestamp = 1718000000000;
    diag.error = std::move(err_info);
    msg.diagnostics = std::vector<ai::DiagnosticEntry>{std::move(diag)};

    // Add usage with cacheWrite1h
    ai::Usage usage;
    usage.input = 100;
    usage.output = 50;
    usage.cache_read = 10;
    usage.cache_write = 20;
    usage.cache_write_1h = 15;
    usage.reasoning = 12;
    usage.total_tokens = 150;
    msg.usage = std::move(usage);

    auto json = ai::glaze::write_message_json(ai::MessageVariant{msg});
    REQUIRE(json);
    CHECK(json->find(R"("diagnostics")") != std::string::npos);
    CHECK(json->find(R"("cacheWrite1h")") != std::string::npos);
    CHECK(json->find(R"("type":"provider_error")") != std::string::npos);

    auto parsed = ai::glaze::read_message_json(*json);
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(*parsed));
    const auto& round_trip = std::get<ai::AssistantMessage>(*parsed);
    REQUIRE(round_trip.diagnostics.has_value());
    REQUIRE(round_trip.diagnostics->size() == 1);
    CHECK((*round_trip.diagnostics)[0].type == "provider_error");
    REQUIRE((*round_trip.diagnostics)[0].error.has_value());
    CHECK((*round_trip.diagnostics)[0].error->name == "NetworkError");
    CHECK(round_trip.usage.cache_write_1h == 15);
    CHECK(round_trip.usage.reasoning == 12);
}

TEST_CASE("context JSON preserves a complete Tool Argument Contract unchanged", "[ai][u2][glaze][issue24]") {
    auto expected_contract = util::read_json<util::JsonValue>(test::kComplexToolArgumentContract);
    REQUIRE(expected_contract);

    ai::AiContext context;
    context.system_prompt = "sys";
    context.model = "gpt-test";
    context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});
    context.tools.push_back(ai::Tool{
        "read_file",
        "Read a workspace file",
        *expected_contract,
    });

    auto json = ai::glaze::write_context_json(context);
    REQUIRE(json);
    auto parsed = ai::glaze::read_context_json(*json);
    REQUIRE(parsed);

    REQUIRE(parsed->system_prompt);
    CHECK(*parsed->system_prompt == "sys");
    CHECK(parsed->model == "gpt-test");
    REQUIRE(parsed->messages.size() == 1);
    REQUIRE(std::holds_alternative<ai::UserMessage>(parsed->messages[0]));
    const auto& user = std::get<ai::UserMessage>(parsed->messages[0]);
    CHECK(ai::text_from_content(user.content) == "hello");
    REQUIRE(parsed->tools.size() == 1);
    CHECK(parsed->tools[0].name == "read_file");

    auto expected_json = util::write_json(*expected_contract);
    auto restored_json = util::write_json(parsed->tools[0].parameters);
    REQUIRE(expected_json);
    REQUIRE(restored_json);
    CHECK(*restored_json == *expected_json);
}

TEST_CASE("ContentDto rejects toolCall for non-assistant context", "[ai][u2][glaze]") {
    ai::glaze::ContentDto dto;
    dto.type = "toolCall";
    dto.id = "call-1";
    dto.name = "read";
    dto.rawArguments = R"({"path":"test"})";

    // detail::content_from_dto rejects toolCall (only valid in assistant content)
    auto result = ai::glaze::detail::content_from_dto(dto, "test");
    REQUIRE(!result);
    CHECK(result.error().message.find("toolCall") != std::string::npos);

    // detail::assistant_content_from_dto accepts toolCall
    auto assistant_result = ai::glaze::detail::assistant_content_from_dto(dto, "test");
    REQUIRE(assistant_result);
    REQUIRE(std::holds_alternative<ai::ToolCallContent>(*assistant_result));
}

TEST_CASE("assistant_content_from_dto rejects image content", "[ai][u2][glaze]") {
    ai::glaze::ContentDto dto;
    dto.type = "image";
    dto.data = "ZmFrZQ==";
    dto.mimeType = "image/png";

    auto result = ai::glaze::detail::assistant_content_from_dto(dto, "test");
    REQUIRE(!result);
    CHECK(result.error().message.find("image") != std::string::npos);
}

