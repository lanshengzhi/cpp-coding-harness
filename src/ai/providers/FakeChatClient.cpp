#include "FakeChatClient.hpp"

#include "../../../src/util/ExpectedMacros.hpp"
#include "../../../include/cch/ai/ChatClient.hpp"
#include "../../../include/cch/ai/Content.hpp"
#include "../../../include/cch/util/Json.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cch::ai::providers {
namespace {

[[nodiscard]] util::Expected<std::string> make_tool_arguments(std::string key, std::string value) {
    util::JsonValue::object_t object;
    object.emplace(std::move(key), util::JsonValue{std::move(value)});
    return util::write_json(util::JsonValue{std::move(object)});
}

class ScriptedStreamingFakeClient final : public ai::StreamingChatClient {
public:
    boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink sink) override {
        ai::AssistantMessage assistant;
        assistant.model = request.model;
        assistant.provider = "fake";
        assistant.api = "scripted-fake";

        if (!request.context.messages.empty()) {
            if (const auto* last_tool = std::get_if<ai::ToolResultMessage>(&request.context.messages.back())) {
                assistant = ai::assistant_text_message("fake observed: " + ai::text_from_content(last_tool->content));
                assistant.provider = "fake";
                assistant.api = "scripted-fake";
                co_return co_await emit_text(std::move(assistant), sink);
            }
        }

        std::string prompt;
        for (auto it = request.context.messages.rbegin(); it != request.context.messages.rend(); ++it) {
            if (const auto* user = std::get_if<ai::UserMessage>(&*it)) {
                prompt = ai::text_from_content(user->content);
                break;
            }
        }

        if (prompt.rfind("read ", 0) == 0) {
            const auto path = prompt.substr(5);
            auto raw = make_tool_arguments("path", path);
            if (!raw) {
                co_return std::unexpected(raw.error());
            }
            auto args = util::read_json<util::JsonValue>(*raw);
            ai::ToolCallContent call;
            call.id = "fake-read-1";
            call.name = "read_file";
            call.raw_arguments = *raw;
            if (args) {
                call.arguments = *args;
            }
            assistant.content.emplace_back(ai::TextContent{"reading " + path, std::nullopt});
            assistant.content.emplace_back(std::move(call));
            assistant.stop_reason = ai::AssistantStopReason::ToolUse;
            if (sink) {
                CCH_TRY_VOID(sink(ai::TextDeltaEvent{0, "reading " + path, assistant}));
            }
            co_return assistant;
        }
        if (prompt.rfind("bash ", 0) == 0) {
            const auto command = prompt.substr(5);
            auto raw = make_tool_arguments("command", command);
            if (!raw) {
                co_return std::unexpected(raw.error());
            }
            auto args = util::read_json<util::JsonValue>(*raw);
            ai::ToolCallContent call;
            call.id = "fake-bash-1";
            call.name = "bash";
            call.raw_arguments = *raw;
            if (args) {
                call.arguments = *args;
            }
            assistant.content.emplace_back(ai::TextContent{"running bash", std::nullopt});
            assistant.content.emplace_back(std::move(call));
            assistant.stop_reason = ai::AssistantStopReason::ToolUse;
            if (sink) {
                CCH_TRY_VOID(sink(ai::TextDeltaEvent{0, "running bash", assistant}));
            }
            co_return assistant;
        }

        assistant = ai::assistant_text_message("fake: " + prompt);
        assistant.provider = "fake";
        assistant.api = "scripted-fake";
        co_return co_await emit_text(std::move(assistant), sink);
    }

private:
    boost::asio::awaitable<util::Expected<ai::AssistantMessage>> emit_text(
        ai::AssistantMessage assistant,
        ai::AssistantEventSink& sink) {
        if (sink && !assistant.content.empty()) {
            auto text = ai::text_from_assistant_content(assistant.content);
            CCH_TRY_VOID(sink(ai::TextDeltaEvent{0, text, assistant}));
        }
        co_return assistant;
    }
};

} // namespace

std::unique_ptr<ai::StreamingChatClient> make_scripted_fake_chat_client() {
    return std::make_unique<ScriptedStreamingFakeClient>();
}

} // namespace cch::ai::providers
