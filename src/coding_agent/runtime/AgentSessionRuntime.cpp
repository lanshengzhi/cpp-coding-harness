#include "AgentSessionRuntime.hpp"

#include <cch/ai/Content.hpp>

#include "agent/AgentPromptAccess.hpp"
#include "coding_agent/SkillFormatting.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/system_executor.hpp>

#include <exception>
#include <optional>
#include <stop_token>
#include <utility>

namespace cch::coding_agent::runtime {

namespace {

[[nodiscard]] std::optional<std::string> last_assistant_text_from(
    const std::vector<ai::MessageVariant>& history) {
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (const auto* am = std::get_if<ai::AssistantMessage>(&*it)) {
            return ai::text_from_assistant_content(am->content);
        }
    }
    return std::nullopt;
}

[[nodiscard]] util::ExpectedVoid prompt_exception(std::exception_ptr exception) {
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& error) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "session prompt coroutine failed",
            error.what()));
    } catch (...) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "session prompt coroutine failed",
            "unknown exception"));
    }
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
    options.model = ai::Model{std::move(config_.model)};

    std::string skills_block = formatSkillsForPrompt(prompt_processor_->skills());
    if (!skills_block.empty()) {
        auto existing_transform = std::move(options.transform_context);
        options.transform_context = [block = std::move(skills_block),
                                     existing = std::move(existing_transform)](
                                        std::vector<ai::MessageVariant> messages,
                                        std::stop_token stop_token) mutable
            -> boost::asio::awaitable<util::Expected<std::vector<ai::MessageVariant>>> {
            std::vector<ai::MessageVariant> transformed;
            if (existing) {
                auto prior = co_await (*existing)(std::move(messages), stop_token);
                if (!prior) {
                    co_return std::unexpected(prior.error());
                }
                transformed = std::move(*prior);
            } else {
                transformed = std::move(messages);
            }

            ai::SystemMessage msg;
            msg.content = block;
            transformed.insert(transformed.begin(), ai::MessageVariant{std::move(msg)});
            co_return transformed;
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
    std::vector<ai::ImageContent> images,
    bool expand_prompt_templates,
    std::move_only_function<util::ExpectedVoid()> on_preflight_accepted) {
    if (auto rejected = reject_if_closed(); !rejected) {
        co_return std::unexpected(rejected.error());
    }
    if (auto rejected = reject_if_busy(); !rejected) {
        co_return std::unexpected(rejected.error());
    }

    state_ = State::RunningPrompt;
    active_stop_source_.emplace();

    util::ExpectedVoid result;
    try {
        bool run_agent = true;
        std::string expanded_prompt;
        if (!prompt_processor_) {
            result = std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "session is closed"));
            run_agent = false;
        } else {
            auto expanded = prompt_processor_->process(
                std::move(prompt), expand_prompt_templates);
            expanded_prompt = std::move(expanded.text);
        }

        if (run_agent && on_preflight_accepted) {
            if (auto acknowledged = on_preflight_accepted(); !acknowledged) {
                result = std::unexpected(acknowledged.error());
                run_agent = false;
            }
        }
        if (run_agent) {
            auto user_message = ai::user_text_message(std::move(expanded_prompt));
            user_message.content.reserve(user_message.content.size() + images.size());
            for (auto& image : images) {
                user_message.content.emplace_back(std::move(image));
            }
            result = co_await run_agent_loop(
                std::move(user_message),
                *active_stop_source_);
        }
    } catch (...) {
        result = prompt_exception(std::current_exception());
    }

    active_stop_source_.reset();
    if (state_ == State::Closing) {
        // The prompt awaitable is the existing observation seam for active
        // close: owned environment cleanup finishes before it settles.
        co_await finalize_close_after_prompt();
    } else if (state_ == State::RunningPrompt) {
        state_ = State::Open;
    }
    co_return result;
}

boost::asio::awaitable<util::ExpectedVoid> AgentSessionRuntime::run_agent_loop(
    ai::UserMessage prompt,
    std::stop_source stop_source) {
    if (!agent_) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session Agent is unavailable"));
    }

    SessionEventCommitment commitment{*session_.store};
    std::optional<util::ExpectedVoid> result;
    result = co_await agent::detail::AgentPromptAccess::prompt(
        *agent_,
        std::move(prompt),
        commitment.sink(),
        std::move(stop_source));
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

namespace {

[[nodiscard]] util::Expected<ai::UserMessage> make_queued_user_message(
    std::optional<prompt::PromptProcessor>& prompt_processor,
    std::string text,
    std::vector<ai::ImageContent> images,
    bool expand_prompt_templates) {
    if (!prompt_processor) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is closed"));
    }
    auto expanded = prompt_processor->process(
        std::move(text), expand_prompt_templates);
    auto message = ai::user_text_message(std::move(expanded.text));
    message.content.reserve(message.content.size() + images.size());
    for (auto& image : images) {
        message.content.emplace_back(std::move(image));
    }
    return message;
}

} // namespace

util::ExpectedVoid AgentSessionRuntime::steer(
    std::string text,
    std::vector<ai::ImageContent> images,
    bool expand_prompt_templates) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    auto message = make_queued_user_message(
        prompt_processor_, std::move(text), std::move(images), expand_prompt_templates);
    if (!message) {
        return std::unexpected(message.error());
    }
    return agent_->steer(ai::MessageVariant{std::move(*message)});
}

util::ExpectedVoid AgentSessionRuntime::follow_up(
    std::string text,
    std::vector<ai::ImageContent> images,
    bool expand_prompt_templates) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    auto message = make_queued_user_message(
        prompt_processor_, std::move(text), std::move(images), expand_prompt_templates);
    if (!message) {
        return std::unexpected(message.error());
    }
    return agent_->follow_up(ai::MessageVariant{std::move(*message)});
}

util::ExpectedVoid AgentSessionRuntime::set_steering_mode(agent::InputQueueMode mode) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    return agent_ ? agent_->set_steering_mode(mode) : std::unexpected(util::make_error(
        util::ErrorCode::Validation, "session is closed"));
}

util::ExpectedVoid AgentSessionRuntime::set_follow_up_mode(agent::InputQueueMode mode) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    return agent_ ? agent_->set_follow_up_mode(mode) : std::unexpected(util::make_error(
        util::ErrorCode::Validation, "session is closed"));
}

util::ExpectedVoid AgentSessionRuntime::clear_steering_queue() {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    return agent_ ? agent_->clear_steering_queue() : std::unexpected(util::make_error(
        util::ErrorCode::Validation, "session is closed"));
}

util::ExpectedVoid AgentSessionRuntime::clear_follow_up_queue() {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    return agent_ ? agent_->clear_follow_up_queue() : std::unexpected(util::make_error(
        util::ErrorCode::Validation, "session is closed"));
}

util::ExpectedVoid AgentSessionRuntime::clear_input_queues() {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    return agent_ ? agent_->clear_input_queues() : std::unexpected(util::make_error(
        util::ErrorCode::Validation, "session is closed"));
}

AgentSessionSnapshot AgentSessionRuntime::snapshot(
    const std::optional<std::filesystem::path>& session_path) const {
    return AgentSessionSnapshot{
        .agent_state = agent_ ? agent_->state() : agent::AgentState{},
        .metadata = session_.metadata,
        .topology = session_.topology,
        .session_path = session_path,
    };
}

std::size_t AgentSessionRuntime::message_count() const {
    return agent_ ? agent_->state().messages.size() : 0;
}

std::optional<std::string> AgentSessionRuntime::last_assistant_text() const {
    return agent_ ? last_assistant_text_from(agent_->state().messages) : std::nullopt;
}

const std::vector<Skill>& AgentSessionRuntime::skills() const {
    static const std::vector<Skill> kEmptySkills;
    return prompt_processor_ ? prompt_processor_->skills() : kEmptySkills;
}

const std::vector<PromptTemplate>& AgentSessionRuntime::templates() const {
    static const std::vector<PromptTemplate> kEmptyTemplates;
    return prompt_processor_ ? prompt_processor_->templates() : kEmptyTemplates;
}

void AgentSessionRuntime::abort() {
    if (state_ == State::RunningPrompt && active_stop_source_) {
        (void)active_stop_source_->request_stop();
    }
}

void AgentSessionRuntime::close() noexcept {
    if (state_ == State::Closing || state_ == State::Closed) {
        return;
    }

    if (state_ == State::RunningPrompt) {
        // Request the same prompt-scoped cancellation as abort(), but retain
        // the active loop, callbacks, commitment, store, and capabilities until
        // run_prompt unwinds after the ordinary aborted lifecycle.
        state_ = State::Closing;
        if (active_stop_source_) {
            (void)active_stop_source_->request_stop();
        }
        return;
    }

    state_ = State::Closing;
    finalize_close();
}

std::shared_ptr<harness::AsyncExecutionEnv>
AgentSessionRuntime::release_close_resources() noexcept {
    if (agent_) {
        agent_->clear_subscriptions();
    }
    agent_.reset();
    prompt_processor_.reset();
    session_.store.reset();
    services_.client.reset();

    if (services_.env_owned) {
        return std::move(services_.env);
    }
    services_.env.reset();
    return {};
}

boost::asio::awaitable<void> AgentSessionRuntime::finalize_close_after_prompt() {
    auto owned_env = release_close_resources();
    if (owned_env) {
        try {
            co_await owned_env->cleanup();
        } catch (...) {
            // cleanup() is best-effort and must not make close fallible.
        }
    }
    state_ = State::Closed;
}

void AgentSessionRuntime::finalize_close() noexcept {
    if (state_ == State::Closed) {
        return;
    }
    auto owned_env = release_close_resources();
    state_ = State::Closed;

    // Idle close has no host executor to await. Transfer the factory-owned
    // environment to a posted best-effort cleanup task; host-owned environments
    // were detached above without cleanup().
    if (owned_env) {
        try {
            // post() prevents a cleanup coroutine from executing inline on the
            // close() stack before its first suspension point.
            boost::asio::post(
                boost::asio::system_executor{},
                [env = std::move(owned_env)]() mutable {
                    try {
                        boost::asio::co_spawn(
                            boost::asio::system_executor{},
                            [env = std::move(env)]() -> boost::asio::awaitable<void> {
                                try {
                                    co_await env->cleanup();
                                } catch (...) {
                                    // cleanup() is best-effort and must not make close fallible.
                                }
                            },
                            boost::asio::detached);
                    } catch (...) {
                        // Launch remains best-effort after close has released ownership.
                    }
                });
        } catch (...) {
            // Scheduling is also best-effort; close remains noexcept.
        }
    }
}

} // namespace cch::coding_agent::runtime
