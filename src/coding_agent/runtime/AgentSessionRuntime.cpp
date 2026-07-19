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

util::ExpectedVoid AgentSessionRuntime::run_prompt(
    std::string prompt,
    bool expand_prompt_templates,
    std::move_only_function<util::ExpectedVoid()> on_preflight_accepted) {
    if (state_ == State::Closed) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is closed"));
    }
    if (state_ == State::RunningPrompt) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is busy (prompt already in flight)"));
    }

    state_ = State::RunningPrompt;
    ScopeExit restore_state{[this] {
        if (state_ != State::Closed) {
            state_ = State::Open;
        }
    }};

    auto expanded = prompt_processor_.process(std::move(prompt), expand_prompt_templates);
    if (on_preflight_accepted) {
        if (auto acknowledged = on_preflight_accepted(); !acknowledged) {
            return acknowledged;
        }
    }
    return run_agent_loop(std::move(expanded.text));
}

util::ExpectedVoid AgentSessionRuntime::run_agent_loop(std::string prompt) {
    boost::asio::io_context io;
    std::optional<util::Expected<agent::AsyncAgentRunResult>> result;

    SessionEventCommitment commitment{session_.history, subscribers_, *session_.store};

    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await loop_->continue_with(
                session_.history, std::move(prompt), commitment.sink());
            co_return;
        },
        boost::asio::detached);
    io.run();

    return commitment.conclude(std::move(result));
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
