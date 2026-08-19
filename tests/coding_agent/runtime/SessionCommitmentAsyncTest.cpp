#include "coding_agent/AgentSession.hpp"

#include "ai/ModelStreamBridge.hpp"
#include <cch/agent/harness/session/SessionStore.hpp>
#include "ai/providers/FakeProvider.hpp"
#include "agent/harness/session/SessionJournalTestHooks.hpp"
#include "support/GatedChatProvider.hpp"
#include "support/PumpUntil.hpp"
#include "support/TempWorkspace.hpp"
#include "support/UserBashTestHooks.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <format>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

using tests::drain_ready;
using tests::PromptResult;
using tests::pump_until;
using tests::spawn_prompt;

struct PersistentSession {
    tests::TempWorkspace workspace;
    std::filesystem::path session_path = workspace.path() / "session.jsonl";

    [[nodiscard]] tests::ModelsSessionOptions options(
        std::shared_ptr<ai::Provider> provider) const {
        tests::ModelsSessionOptions options;
        options.session_target =
            coding_agent::ExplicitOpenOrCreateSessionTarget{session_path};
        options.workspace = workspace.path();
        options.models = tests::models_from_provider(std::move(provider));
        return options;
    }

    [[nodiscard]] std::vector<std::string> persisted_texts() const {
        auto loaded = harness::session::SessionStore::load(session_path);
        REQUIRE(loaded.has_value());
        std::vector<std::string> texts;
        for (const auto& message : loaded->messages) {
            if (const auto* user = std::get_if<ai::UserMessage>(&message)) {
                texts.push_back(ai::text_from_user_message(*user));
            } else if (const auto* assistant =
                           std::get_if<ai::AssistantMessage>(&message)) {
                texts.push_back(ai::text_from_assistant_content(assistant->content));
            }
        }
        return texts;
    }
};

/// Abort-aware gated provider: blocks every request until release(), but a
/// requested stop (session.abort) cancels the gate so the run settles with
/// the authoritative aborted Assistant Message (pi: the aborted Assistant
/// Message is the run's terminal). The stop-ignorant shared GatedChatProvider
/// stays for the overlap tests that must hold the run open across Close.
class AbortAwareGatedChatProvider final : public tests::ScriptedProvider {
public:
    AbortAwareGatedChatProvider() : ScriptedProvider("sdk-host") {}

    [[nodiscard]] ai::ModelStream stream(
        ai::Model model,
        ai::AiContext context,
        ai::ProviderStreamOptions options) override {
        return ai::detail::make_model_stream(
            [this,
             model = std::move(model),
             context = std::move(context),
             options = std::move(options)](
                ai::AssistantEventSink)
                -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                requests.push_back(
                    tests::RecordedProviderRequest{model, context, options});
                const auto turn = requests.size();

                const auto executor = co_await boost::asio::this_coro::executor;
                const auto stop_token = options.stop_token;
                gate_.emplace(executor);
                gate_->expires_at(std::chrono::steady_clock::time_point::max());
                std::stop_callback cancellation{stop_token, [this] {
                    if (gate_) (void)gate_->cancel();
                }};
                boost::system::error_code error;
                if (!stop_token.stop_requested()) {
                    co_await gate_->async_wait(
                        boost::asio::redirect_error(boost::asio::use_awaitable, error));
                }
                gate_.reset();

                auto response = ai::assistant_text_message(std::format("turn {}", turn));
                response.provider = "gated-fake";
                response.api = "fake";
                response.model = model.id;
                response.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                if (stop_token.stop_requested()) {
                    response.stop_reason = ai::AssistantStopReason::Aborted;
                    response.error_message = "prompt aborted";
                }
                co_return response;
            });
    }

    void release() {
        if (gate_) (void)gate_->cancel();
    }

    std::vector<tests::RecordedProviderRequest> requests;

private:
    std::optional<boost::asio::steady_timer> gate_;
};

} // namespace

TEST_CASE(
    "an admitted event advances live Session state before weak observers are notified",
    "[coding_agent][runtime][commitment][issue464]") {
    PersistentSession fixture;
    auto created = coding_agent::create_agent_session(
        fixture.options(ai::providers::make_scripted_fake_provider()));
    REQUIRE(created.has_value());

    auto* session = created->session.get();
    std::vector<std::size_t> counts_at_message_end;
    auto subscription = session->subscribe(
        [session, &counts_at_message_end](const agent::AgentLifecycleEvent& event)
            -> support::ExpectedVoid {
            if (std::holds_alternative<agent::MessageEndEvent>(event)) {
                counts_at_message_end.push_back(session->message_count());
            }
            return {};
        });
    REQUIRE(subscription.has_value());

    REQUIRE(session->prompt_blocking("observe order").has_value());

    // The weak observer of each message end already saw the message in live
    // Session state (user first, assistant second).
    CHECK(
        counts_at_message_end ==
        std::vector<std::size_t>{1, 2});
    session->close();
}

TEST_CASE(
    "session event persistence executes off the interaction loop in admission order",
    "[coding_agent][runtime][commitment][issue464]") {
    PersistentSession fixture;
    auto created = coding_agent::create_agent_session(
        fixture.options(ai::providers::make_scripted_fake_provider()));
    REQUIRE(created.has_value());
    auto& session = *created->session;

    harness::session::testing::record_append_threads_for_test(fixture.session_path);
    const auto driving_thread = std::this_thread::get_id();

    REQUIRE(session.prompt_blocking("first").has_value());
    REQUIRE(session.prompt_blocking("second").has_value());

    const auto append_threads =
        harness::session::testing::recorded_append_threads_for_test(
            fixture.session_path);
    // user + assistant message per prompt.
    REQUIRE(append_threads.size() == 4);
    for (const auto& thread : append_threads) {
        CHECK(thread != driving_thread);
    }

    CHECK(
        fixture.persisted_texts() ==
        std::vector<std::string>{
            "first", "fake: first", "second", "fake: second"});
    session.close();
}

TEST_CASE(
    "a persistence failure keeps live state and rejects later prompts with a typed failure",
    "[coding_agent][runtime][commitment][issue464]") {
    PersistentSession fixture;
    auto created = coding_agent::create_agent_session(
        fixture.options(ai::providers::make_scripted_fake_provider()));
    REQUIRE(created.has_value());
    auto& session = *created->session;

    // Fresh sessions write one line per append: the user message lands, the
    // assistant message write fails.
    harness::session::testing::fail_nth_append_for_test(fixture.session_path, 2);
    auto failed = session.prompt_blocking("keep live state");
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == support::ErrorCode::Session);
    CHECK(failed.error().message == "could not persist session entry");

    // No rollback theatre: the observed live state stays.
    CHECK(session.is_open());
    CHECK(session.message_count() == 2);

    // Later prompts are rejected with the typed session failure.
    auto rejected = session.prompt_blocking("rejected prompt");
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == support::ErrorCode::Session);
    CHECK(
        rejected.error().message ==
        "session persistence failed; rejecting new prompt");
    CHECK(session.message_count() == 2);

    session.close();
    CHECK(fixture.persisted_texts() == std::vector<std::string>{"keep live state"});
}

TEST_CASE(
    "an aborted prompt settles the commitment channel with a consistent session file",
    "[coding_agent][runtime][commitment][issue464]") {
    PersistentSession fixture;
    auto provider = std::make_shared<AbortAwareGatedChatProvider>();
    auto* gated = provider.get();
    auto created = coding_agent::create_agent_session(fixture.options(std::move(provider)));
    REQUIRE(created.has_value());
    auto& session = *created->session;

    boost::asio::io_context io;
    PromptResult prompt_result;
    spawn_prompt(io, session, "abort me", prompt_result);
    drain_ready(io);
    REQUIRE(gated->requests.size() == 1);

    session.abort();
    gated->release();
    REQUIRE(pump_until(io, [&] { return prompt_result.has_value(); }));
    // The aborted run resolves through the ordinary prompt verdict (pi
    // semantics: the aborted Assistant Message is the terminal).
    CHECK(prompt_result->has_value());

    const auto snapshot = session.snapshot();
    REQUIRE(snapshot.agent_state.messages.size() == 2);
    const auto& aborted =
        std::get<ai::AssistantMessage>(snapshot.agent_state.messages[1]);
    CHECK(aborted.stop_reason == ai::AssistantStopReason::Aborted);

    // Every admitted event persisted in order before the prompt settled.
    CHECK(fixture.persisted_texts().front() == "abort me");
    session.close();
}

TEST_CASE(
    "interaction and timers progress while session persistence is slow",
    "[coding_agent][runtime][commitment][issue464]") {
    PersistentSession fixture;
    auto created = coding_agent::create_agent_session(
        fixture.options(ai::providers::make_scripted_fake_provider()));
    REQUIRE(created.has_value());
    auto& session = *created->session;

    harness::session::testing::delay_appends_for_test(
        fixture.session_path, std::chrono::milliseconds{300});

    boost::asio::io_context io;
    PromptResult prompt_result;
    spawn_prompt(io, session, "slow persistence", prompt_result);

    bool timer_fired = false;
    bool timer_beat_prompt = false;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            boost::asio::steady_timer timer(
                co_await boost::asio::this_coro::executor,
                std::chrono::milliseconds{50});
            co_await timer.async_wait(boost::asio::use_awaitable);
            timer_fired = true;
            timer_beat_prompt = !prompt_result.has_value();
        },
        boost::asio::detached);

    REQUIRE(pump_until(io, [&] { return prompt_result.has_value(); }));
    harness::session::testing::clear_append_delay_for_test(fixture.session_path);

    CHECK(prompt_result->has_value());
    CHECK(timer_fired);
    CHECK(timer_beat_prompt);
    CHECK(
        fixture.persisted_texts() ==
        std::vector<std::string>{"slow persistence", "fake: slow persistence"});
    session.close();
}

TEST_CASE(
    "a second session on the same Runtime root completes while persistence is slow",
    "[coding_agent][runtime][commitment][issue464]") {
    PersistentSession slow_fixture;
    PersistentSession fast_fixture;
    auto slow_created = coding_agent::create_agent_session(
        slow_fixture.options(ai::providers::make_scripted_fake_provider()));
    auto fast_created = coding_agent::create_agent_session(
        fast_fixture.options(ai::providers::make_scripted_fake_provider()));
    REQUIRE(slow_created.has_value());
    REQUIRE(fast_created.has_value());
    auto& slow_session = *slow_created->session;
    auto& fast_session = *fast_created->session;

    harness::session::testing::delay_appends_for_test(
        slow_fixture.session_path, std::chrono::milliseconds{300});

    boost::asio::io_context io;
    PromptResult slow_result;
    PromptResult fast_result;
    spawn_prompt(io, slow_session, "slow session", slow_result);
    drain_ready(io);
    spawn_prompt(io, fast_session, "fast session", fast_result);

    // The fast session's prompt completes without waiting for the slow
    // session's persistence chain.
    REQUIRE(pump_until(io, [&] { return fast_result.has_value(); }));
    CHECK(fast_result->has_value());

    REQUIRE(pump_until(io, [&] { return slow_result.has_value(); }));
    harness::session::testing::clear_append_delay_for_test(slow_fixture.session_path);
    CHECK(slow_result->has_value());

    slow_session.close();
    fast_session.close();
}
