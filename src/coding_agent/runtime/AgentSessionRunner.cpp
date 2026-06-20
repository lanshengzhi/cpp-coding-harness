#include "AgentSessionRunner.hpp"

#include "../../../include/cch/ai/Content.hpp"
#include "../../../include/cch/coding_agent/SkillFormatting.hpp"
#include "../../harness/WorkspaceFileSystem.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <optional>
#include <utility>

namespace cch::coding_agent::runtime {

std::string terminal_code_for_loop_error(const std::string& message) {
    if (message == "max turns exceeded") {
        return "max_turns_exceeded";
    }
    return "runtime_error";
}

std::string display_message_for_loop_error(const std::string& message) {
    return message == "max turns exceeded" ? "max_turns_exceeded" : message;
}

AgentSessionRunner::AgentSessionRunner(
    ai::StreamingChatClient& client,
    agent::AsyncToolRegistry registry,
    agent::AsyncAgentOptions options,
    std::vector<PromptTemplate> templates,
    CommandRegistry* command_registry,
    std::vector<Skill> skills)
    : templates_(std::move(templates)),
      command_registry_(command_registry),
      skills_(std::move(skills)) {
    // U4: Build the <available_skills> block once at construction time and
    // inject it into the agent context via the get_steering_messages hook.
    // This is called on every turn — the block is static and cheap to emit.
    std::string skills_block = formatSkillsForPrompt(skills_);
    if (!skills_block.empty()) {
        options.get_steering_messages = [block = std::move(skills_block)]()
            -> util::Expected<std::vector<ai::MessageVariant>> {
            ai::UserMessage msg;
            msg.content.emplace_back(ai::TextContent{block});
            std::vector<ai::MessageVariant> msgs;
            msgs.push_back(ai::MessageVariant{std::move(msg)});
            return msgs;
        };
    }
    // Construct loop_ last — it takes ownership of options (move-only)
    loop_.emplace(client, std::move(registry), std::move(options));
}

PromptRunResult AgentSessionRunner::run_prompt(
    std::vector<ai::MessageVariant>& history,
    harness::session::JsonlSessionStore& store,
    std::string prompt,
    agent::AgentEventSink sink) {
    // Process slash-commands and prompt templates before the agent loop
    CommandContext cmd_ctx{
        .session_id = store.metadata().session_id,
        .workspace_path = store.metadata().workspace.string(),
        .provider = store.metadata().provider,
        .model = store.metadata().model,
        .message_count = history.size(),
    };
    auto processed = process_prompt(prompt, templates_,
        command_registry_ ? *command_registry_ : CommandRegistry::empty(),
        cmd_ctx,
        skills_,
        harness::WorkspaceFileSystem{store.metadata().workspace});
    if (processed.command_handled) {
        if (processed.shutdown_requested) {
            return PromptRunResult{true, "shutdown", {}};
        }
        return PromptRunResult{true, "command_handled", processed.display_text.value_or("")};
    }
    prompt = std::move(processed.expanded_prompt);

    boost::asio::io_context io;
    std::optional<util::Expected<agent::AsyncAgentRunResult>> result;
    const auto previous_size = history.size();

    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await loop_->continue_with(history, std::move(prompt), std::move(sink));
            co_return;
        },
        boost::asio::detached);
    io.run();

    if (!result || !*result) {
        const auto message = result ? (*result).error().message : std::string{"async loop did not finish"};
        return PromptRunResult{
            false,
            terminal_code_for_loop_error(message),
            display_message_for_loop_error(message),
        };
    }

    auto new_history = std::move((*result)->context.messages);
    for (std::size_t index = previous_size; index < new_history.size(); ++index) {
        if (auto appended = store.append(new_history[index]); !appended) {
            return PromptRunResult{false, "session_persist_failed", "could not persist session entry"};
        }
    }

    history = std::move(new_history);
    return PromptRunResult{true, "completed", {}};
}

} // namespace cch::coding_agent::runtime
