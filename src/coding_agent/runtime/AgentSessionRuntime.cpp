#include "AgentSessionRuntime.hpp"

#include "../../../include/cch/ai/Content.hpp"
#include "coding_agent/SkillFormatting.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <optional>
#include <utility>
#include <variant>

namespace cch::coding_agent::runtime {

namespace {

[[nodiscard]] std::string terminal_code_for_loop_error(const std::string& message) {
    if (message == "max turns exceeded") {
        return "max_turns_exceeded";
    }
    if (message == "agent event sink failed") {
        return "event_sink_failed";
    }
    return "runtime_error";
}

[[nodiscard]] std::string display_message_for_loop_error(const std::string& message) {
    return message == "max turns exceeded" ? "max_turns_exceeded" : message;
}

template <typename Callback>
class ScopeExit final {
public:
    explicit ScopeExit(Callback callback) : callback_(std::move(callback)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit() { callback_(); }

private:
    Callback callback_;
};

[[nodiscard]] std::optional<std::string> last_assistant_text_from(
    const std::vector<ai::MessageVariant>& history) {
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (const auto* am = std::get_if<ai::AssistantMessage>(&*it)) {
            return ai::text_from_assistant_content(am->content);
        }
    }
    return std::nullopt;
}

} // namespace

AgentSessionRuntime::AgentSessionRuntime(
    RuntimeServices services,
    OpenSession session,
    prompt::PromptProcessor prompt_processor,
    AgentSessionRuntimeConfig config)
    : services_(std::move(services)),
      session_(std::move(session)),
      prompt_processor_(std::move(prompt_processor)),
      config_(std::move(config)) {
    // Build the <available_skills> block once from the same immutable snapshot
    // used for explicit /skill:name invocation.
    agent::AsyncAgentOptions options;
    options.max_turns = config_.max_turns > 0 ? config_.max_turns : 30;
    options.model = std::move(config_.model);

    std::string skills_block = formatSkillsForPrompt(prompt_processor_.skills());
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

    // Construct loop_ last — it takes ownership of tools (move-only).
    loop_.emplace(*services_.client, std::move(services_.tools), std::move(options));
}

PromptRunResult AgentSessionRuntime::run_prompt(
    std::string prompt,
    bool expand_prompt_templates,
    agent::AgentEventSink sink) {
    if (state_ == State::Closed) {
        return PromptRunResult{false, "session_closed", "session is closed", {}};
    }
    if (state_ == State::RunningPrompt) {
        return PromptRunResult{false, "session_busy", "session is busy (prompt already in flight)", {}};
    }

    state_ = State::RunningPrompt;
    ScopeExit restore_state{[this] {
        if (state_ != State::Closed) {
            state_ = State::Open;
        }
    }};

    prompt::PromptProcessingOutcome processing = prompt::AgentPrompt{std::move(prompt)};
    if (expand_prompt_templates) {
        auto* raw_prompt = std::get_if<prompt::AgentPrompt>(&processing);
        processing = prompt_processor_.process(
            std::move(raw_prompt->text),
            CommandContext{
                .session_id = session_.metadata.session_id,
                .workspace_path = session_.workspace.string(),
                .provider = session_.metadata.provider,
                .model = session_.metadata.model,
                .message_count = session_.history.size(),
                .available_commands = {},
            });
    }

    if (auto* handled = std::get_if<prompt::CommandHandled>(&processing)) {
        return PromptRunResult{
            .success = true,
            .code = std::move(handled->code),
            .message = std::move(handled->feedback),
            .diagnostics = {},
        };
    }

    auto combined_sink = make_combined_sink(std::move(sink));
    return run_agent_loop(
        std::move(std::get<prompt::AgentPrompt>(processing).text),
        std::move(combined_sink));
}

PromptRunResult AgentSessionRuntime::run_agent_loop(
    std::string prompt,
    agent::AgentEventSink sink) {
    boost::asio::io_context io;
    std::optional<util::Expected<agent::AsyncAgentRunResult>> result;
    const auto previous_size = session_.history.size();

    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await loop_->continue_with(session_.history, std::move(prompt), std::move(sink));
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
            {},
        };
    }

    auto new_history = std::move((*result)->context.messages);
    for (std::size_t index = previous_size; index < new_history.size(); ++index) {
        if (auto appended = session_.store->append(new_history[index]); !appended) {
            return PromptRunResult{false, "session_persist_failed", "could not persist session entry", {}};
        }
    }

    session_.history = std::move(new_history);
    return PromptRunResult{true, "completed", {}, {}};
}

agent::AgentEventSink AgentSessionRuntime::make_combined_sink(agent::AgentEventSink per_prompt) {
    auto persistent = std::make_shared<std::vector<agent::AgentEventSink*>>();
    for (auto& sub : subscribers_) {
        if (sub.active && sub.sink) {
            persistent->push_back(&sub.sink);
        }
    }

    return [persistent = std::move(persistent),
            per_prompt = std::move(per_prompt)](const agent::AgentLifecycleEvent& event) mutable
        -> util::ExpectedVoid {
        for (auto* sink : *persistent) {
            if (*sink) {
                auto r = (*sink)(event);
                if (!r) return r;
            }
        }
        if (per_prompt) {
            return per_prompt(event);
        }
        return {};
    };
}

int AgentSessionRuntime::subscribe(agent::AgentEventSink sink) {
    if (state_ == State::Closed) {
        return -1;
    }
    int id = next_subscriber_id_++;
    subscribers_.push_back({id, std::move(sink), true});
    return id;
}

void AgentSessionRuntime::unsubscribe(int id) {
    for (auto& sub : subscribers_) {
        if (sub.id == id) {
            sub.active = false;
            return;
        }
    }
}

bool AgentSessionRuntime::is_subscribed(int id) const {
    for (const auto& sub : subscribers_) {
        if (sub.id == id) {
            return sub.active;
        }
    }
    return false;
}

std::optional<std::string> AgentSessionRuntime::last_assistant_text() const {
    return last_assistant_text_from(session_.history);
}

void AgentSessionRuntime::close() {
    if (state_ == State::Closed) {
        return;
    }
    state_ = State::Closed;

    for (auto& sub : subscribers_) {
        sub.active = false;
    }
    subscribers_.clear();

    loop_.reset();

    // Best-effort async cleanup of the execution environment only when the
    // factory owns it. Host-provided shared environments must outlive the
    // session and are never cleaned up here.
    if (services_.env && services_.env_owned) {
        boost::asio::io_context io;
        boost::asio::co_spawn(io, services_.env->cleanup(), boost::asio::detached);
        io.run();
    }
}

} // namespace cch::coding_agent::runtime
