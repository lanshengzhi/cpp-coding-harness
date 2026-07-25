#include "AgentSessionRuntime.hpp"

#include "../../../include/cch/ai/Content.hpp"
#include "coding_agent/SkillFormatting.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <optional>
#include <utility>

namespace cch::coding_agent::runtime {

namespace {

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
    options.max_turns = config_.max_turns;
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

    // Resumed history is transferred exactly once into the authoritative Agent
    // state. AgentSession retains product metadata and durable storage only.
    agent::AgentInitialState initial_state;
    initial_state.messages = std::move(session_.history);
    initial_state.thinking_level = session_.context_thinking_level.value_or("off");

    // Construct Agent last: it borrows the factory-owned client and takes sole
    // ownership of the move-only tool registry.
    agent_.emplace(
        *services_.client,
        std::move(services_.tools),
        std::move(options),
        std::move(initial_state));
}

util::ExpectedVoid AgentSessionRuntime::reject_if_closed() const {
    if (state_ == State::Closing || state_ == State::Closed) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is closed"));
    }
    return {};
}

util::ExpectedVoid AgentSessionRuntime::reject_if_busy() const {
    if (state_ == State::RunningPrompt) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is busy (prompt already in flight)"));
    }
    return {};
}

boost::asio::awaitable<util::ExpectedVoid> AgentSessionRuntime::run_prompt(
    std::string prompt,
    bool expand_prompt_templates,
    std::move_only_function<util::ExpectedVoid()> on_preflight_accepted) {
    if (auto rejected = reject_if_closed(); !rejected) {
        co_return std::unexpected(rejected.error());
    }
    if (auto rejected = reject_if_busy(); !rejected) {
        co_return std::unexpected(rejected.error());
    }

    state_ = State::RunningPrompt;
    ScopeExit restore_state{[this] {
        if (state_ == State::Closing) {
            finalize_close();
        } else if (state_ == State::RunningPrompt) {
            state_ = State::Open;
        }
    }};

    auto expanded = prompt_processor_.process(std::move(prompt), expand_prompt_templates);
    if (on_preflight_accepted) {
        if (auto acknowledged = on_preflight_accepted(); !acknowledged) {
            co_return acknowledged;
        }
    }
    co_return co_await run_agent_loop(std::move(expanded.text));
}

boost::asio::awaitable<util::ExpectedVoid> AgentSessionRuntime::run_agent_loop(
    std::string prompt) {
    if (!agent_) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session Agent is unavailable"));
    }

    SessionEventCommitment commitment{*session_.store};
    std::optional<util::ExpectedVoid> result;
    result = co_await agent_->prompt(
        std::move(prompt), commitment.sink());
    co_return commitment.conclude(std::move(result));
}

util::Expected<agent::AgentEventSubscription> AgentSessionRuntime::subscribe(
    agent::AgentEventSink sink) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    if (!agent_) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is closed"));
    }
    return agent_->subscribe(std::move(sink));
}

std::size_t AgentSessionRuntime::message_count() const {
    return agent_ ? agent_->state().messages.size() : 0;
}

std::optional<std::string> AgentSessionRuntime::last_assistant_text() const {
    return agent_ ? last_assistant_text_from(agent_->state().messages) : std::nullopt;
}

void AgentSessionRuntime::close() {
    if (state_ == State::Closing || state_ == State::Closed) {
        return;
    }

    if (agent_) {
        agent_->clear_subscriptions();
    }
    if (state_ == State::RunningPrompt) {
        // The active Agent coroutine, strong commitment, store, client, tools,
        // and execution environment remain valid until run_prompt unwinds.
        state_ = State::Closing;
        return;
    }

    state_ = State::Closing;
    finalize_close();
}

void AgentSessionRuntime::finalize_close() {
    if (state_ == State::Closed) {
        return;
    }
    state_ = State::Closed;
    agent_.reset();

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
