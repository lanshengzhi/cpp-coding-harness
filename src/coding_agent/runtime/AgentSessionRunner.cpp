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
    if (message == "agent event sink failed") {
        return "event_sink_failed";
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
    // Build the <available_skills> block once at construction time and inject
    // it into each provider request via transform_context. Unlike queued
    // steering messages, this does not mutate conversation history or force an
    // extra turn after the assistant stops.
    std::string skills_block = formatSkillsForPrompt(skills_);
    if (!skills_block.empty()) {
        auto existing_transform = std::move(options.transform_context);
        options.transform_context = [block = std::move(skills_block),
                                        existing = std::move(existing_transform)](
                                        const std::vector<ai::MessageVariant>& messages) mutable
            -> util::Expected<std::vector<ai::MessageVariant>> {
            std::vector<ai::MessageVariant> transformed;
            if (existing) {
                auto prior = (*existing)(messages);
                if (!prior) {
                    return std::unexpected(prior.error());
                }
                transformed = std::move(*prior);
            } else {
                transformed = messages;
            }

            ai::SystemMessage msg;
            msg.content = block;
            transformed.insert(transformed.begin(), ai::MessageVariant{std::move(msg)});
            return transformed;
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
