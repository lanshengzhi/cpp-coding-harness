#include "AsyncCliRuntime.hpp"

#include <cch/agent/AgentLoop.hpp>
#include <cch/ai/providers/BoostBeastStreamTransport.hpp>
#include <cch/ai/providers/OpenAIChatClient.hpp>
#include <cch/harness/LocalExecutionEnv.hpp>
#include <cch/tools/ToolFactories.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <glaze/glaze.hpp>

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cch::cli {
namespace {

std::string text_from_async_content(const std::vector<ai::Content>& content) {
    std::string text;
    for (const auto& block : content) {
        if (const auto* text_block = std::get_if<ai::TextContent>(&block)) {
            text += text_block->text;
        }
    }
    return text;
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
                assistant = ai::assistant_text_message("fake observed: " + text_from_async_content(last_tool->content));
                assistant.provider = "fake";
                co_return co_await emit_text(std::move(assistant), sink);
            }
        }

        std::string prompt;
        for (auto it = request.context.messages.rbegin(); it != request.context.messages.rend(); ++it) {
            if (const auto* user = std::get_if<ai::UserMessage>(&*it)) {
                prompt = text_from_async_content(user->content);
                break;
            }
        }

        if (prompt.rfind("read ", 0) == 0) {
            const auto path = prompt.substr(5);
            auto raw = "{\"path\":\"" + path + "\"}";
            auto args = util::read_json<glz::generic>(raw);
            ai::ToolCallContent call;
            call.id = "fake-read-1";
            call.name = "read_file";
            call.raw_arguments = raw;
            if (args) call.arguments = *args;
            assistant.content.emplace_back(ai::TextContent{"reading " + path, std::nullopt});
            assistant.content.emplace_back(std::move(call));
            assistant.stop_reason = ai::AssistantStopReason::ToolUse;
            if (sink) sink(ai::TextDeltaEvent{0, "reading " + path, assistant});
            co_return assistant;
        }
        if (prompt.rfind("bash ", 0) == 0) {
            const auto command = prompt.substr(5);
            auto raw = "{\"command\":\"" + command + "\"}";
            auto args = util::read_json<glz::generic>(raw);
            ai::ToolCallContent call;
            call.id = "fake-bash-1";
            call.name = "bash";
            call.raw_arguments = raw;
            if (args) call.arguments = *args;
            assistant.content.emplace_back(ai::TextContent{"running bash", std::nullopt});
            assistant.content.emplace_back(std::move(call));
            assistant.stop_reason = ai::AssistantStopReason::ToolUse;
            if (sink) sink(ai::TextDeltaEvent{0, "running bash", assistant});
            co_return assistant;
        }

        assistant = ai::assistant_text_message("fake: " + prompt);
        assistant.provider = "fake";
        co_return co_await emit_text(std::move(assistant), sink);
    }

private:
    boost::asio::awaitable<util::Expected<ai::AssistantMessage>> emit_text(
        ai::AssistantMessage assistant,
        const ai::AssistantEventSink& sink) {
        if (sink && !assistant.content.empty()) {
            auto text = text_from_async_content(assistant.content);
            auto emitted = sink(ai::TextDeltaEvent{0, text, assistant});
            if (!emitted) {
                co_return std::unexpected(emitted.error());
            }
        }
        co_return assistant;
    }
};

void print_async_event(const agent::AgentLifecycleEvent& event) {
    if (const auto* turn = std::get_if<agent::TurnStartEvent>(&event)) {
        std::cout << "[model-request] turn " << turn->turn << '\n';
    } else if (const auto* update = std::get_if<agent::MessageUpdateEvent>(&event)) {
        std::cout << "[assistant] " << update->delta << '\n';
    } else if (const auto* start = std::get_if<agent::ToolExecutionStartEvent>(&event)) {
        std::cout << "[tool-call] " << start->tool_name << '#' << start->tool_call_id << '\n';
    } else if (const auto* end = std::get_if<agent::ToolExecutionEndEvent>(&event)) {
        std::cout << (end->is_error ? "[tool-error] " : "[tool-success] ") << end->tool_call_id << '\n';
        if (end->is_error && !end->content.empty()) {
            std::cout << end->content << '\n';
        }
    } else if (const auto* done = std::get_if<agent::AgentEndEvent>(&event)) {
        if (done->success) {
            std::cout << "[completed] " << done->reason << '\n';
        } else if (done->reason == "max turns exceeded") {
            std::cout << "[max-turns] max_turns_exceeded\n";
        }
    }
}

} // namespace

int run_async_cli(const AsyncCliRuntimeConfig& config) {
    std::unique_ptr<ai::StreamingChatClient> client;
    if (config.fake) {
        client = std::make_unique<ScriptedStreamingFakeClient>();
    } else {
        auto transport = std::make_shared<ai::providers::BoostBeastStreamTransport>();
        ai::providers::OpenAIStreamConfig provider;
        provider.base_url = config.base_url;
        provider.api_key_env = config.api_key_env;
        provider.model = config.model;
        client = std::make_unique<ai::providers::StreamingOpenAIChatClient>(transport, provider);
    }

    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(
        config.workspace,
        config.enable_bash,
        std::vector<std::string>{config.api_key_env});
    agent::AsyncToolRegistry registry;
    registry.add(tools::make_async_read_file_tool(env));
    registry.add(tools::make_async_write_file_tool(env));
    registry.add(tools::make_async_edit_file_tool(env));
    registry.add(tools::make_async_bash_tool(env));
    agent::AsyncAgentLoop loop(*client, std::move(registry), agent::AsyncAgentOptions{config.max_turns, config.model});

    auto run_prompt = [&](const std::string& prompt) -> bool {
        boost::asio::io_context io;
        std::optional<util::Expected<agent::AsyncAgentRunResult>> result;
        boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                result = co_await loop.run(prompt, [](const agent::AgentLifecycleEvent& event) {
                    print_async_event(event);
                    return util::ExpectedVoid{};
                });
                co_return;
            },
            boost::asio::detached);
        io.run();
        if (!result || !*result) {
            const auto message = result ? (*result).error().message : std::string{"async loop did not finish"};
            std::cerr << "loop failed: " << (message == "max turns exceeded" ? "max_turns_exceeded" : message) << '\n';
            return false;
        }
        const auto& final_message = std::get<ai::AssistantMessage>((*result)->context.messages.back());
        std::cout << text_from_async_content(final_message.content) << '\n';
        return true;
    };

    if (config.repl) {
        std::string line;
        while (std::cout << "> " && std::getline(std::cin, line)) {
            if (line == "exit" || line == "quit") break;
            if (!line.empty() && !run_prompt(line)) return 1;
        }
        return 0;
    }

    return run_prompt(config.prompt) ? 0 : 1;
}

} // namespace cch::cli
