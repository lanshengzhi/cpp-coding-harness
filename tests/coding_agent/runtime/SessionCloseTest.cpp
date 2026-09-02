// Session Close quiescence evidence for #467: an idempotent Close stops new
// prompt and work admission before requesting cancellation, waits for every
// admitted prompt, User Bash, Session Event Commitment, and manual compaction
// to reach its terminal outcome before releasing Session resources, and
// leaves late callbacks and subscriptions as suppressed or benign drops
// (ADR 0011 two-phase close; ADR 0040 §Session Event Commitment and Close).
// No live keys or network: every request is served by scripted fake seams.

#include "ai/ModelStreamBridge.hpp"
#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/coding_agent/AgentSessionEvent.hpp>
#include "coding_agent/AgentSession.hpp"
#include <cch/agent/harness/session/SessionStore.hpp>
#include "agent/harness/session/SessionJournalTestHooks.hpp"
#include <cch/support/Error.hpp>
#include "support/GatedChatProvider.hpp"
#include "support/ModelsFixture.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
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
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <optional>
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

template <typename T>
[[nodiscard]] T run_awaitable(boost::asio::awaitable<T> awaitable) {
    boost::asio::io_context io;
    std::optional<T> result;
    std::exception_ptr exception;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            try {
#endif
                result = co_await std::move(awaitable);
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            } catch (...) {
                exception = std::current_exception();
            }
#endif
            co_return;
        },
        boost::asio::detached);
    io.run();
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    if (exception) {
        std::rethrow_exception(exception);
    }
#endif
    REQUIRE(result.has_value());
    return std::move(*result);
}

struct CloseFixture {
    tests::TempWorkspace workspace;
    std::filesystem::path session_path = workspace.path() / "close.jsonl";
    tests::RuntimeFixture runtime;

    [[nodiscard]] tests::ModelsSessionOptions options(std::shared_ptr<tests::ScriptedProvider> provider) const {
        tests::ModelsSessionOptions options;
        options.session_target =
            coding_agent::ExplicitOpenOrCreateSessionTarget{session_path};
        options.workspace = workspace.path();
        options.execution_runtime_target = runtime.make_target();
        auto model_runtime = tests::runtime_from_provider(std::move(provider));
        if (!model_runtime) {
            std::terminate();
        }
        options.model_runtime = std::move(*model_runtime);
        options.request_model = tests::scripted_request_model("fake", "fake-model");
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
                texts.push_back(
                    ai::text_from_assistant_content(assistant->content));
            }
        }
        return texts;
    }
};

/// FIFO scripted provider with an optional gate on one request number: the
/// gated request blocks until `release_gate()`. A requested stop after the
/// gate completes with the aborted terminal (pi: the aborted Assistant
/// Message is the run's terminal); the compaction summarization stream carries
/// a never-stopped token, so a gated summarization always completes — Close
/// awaits admitted compaction work rather than cancelling it.
class GatedScriptedProvider final : public tests::ScriptedProvider {
public:
    GatedScriptedProvider() : ScriptedProvider("sdk-host") {}

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext context, coding_agent::ModelRuntimeTestStreamOptions options) override {
        return ai::detail::make_model_stream(
            [this,
             model = std::move(model),
             context = std::move(context),
             options = std::move(options)](
                ai::AssistantEventSink sink) mutable
                -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                ++request_count;
                requests.push_back(tests::RecordedProviderRequest{
                    .model = model,
                    .context = context,
                    .options = options,
                });
                if (gate_request_number &&
                    request_count == *gate_request_number) {
                    gate_.emplace(co_await boost::asio::this_coro::executor);
                    gate_->expires_at(
                        std::chrono::steady_clock::time_point::max());
                    boost::system::error_code error;
                    co_await gate_->async_wait(boost::asio::redirect_error(
                        boost::asio::use_awaitable, error));
                    gate_.reset();
                }
                if (options.stop_token.stop_requested()) {
                    auto terminal = ai::assistant_text_message("");
                    terminal.stop_reason = ai::AssistantStopReason::Aborted;
                    terminal.error_message = "Request was aborted";
                    terminal.provider = "sdk-host";
                    terminal.api = "fake";
                    terminal.model = model.id;
                    terminal.timestamp = 1718000000123;
                    if (sink) {
                        if (auto failed = sink(ai::AssistantErrorEvent{
                                .reason = terminal.stop_reason,
                                .error = terminal,
                                .failure = support::make_error(
                                    support::ErrorCode::Cancelled,
                                    "Request was aborted"),
                            });
                            !failed) {
                            co_return std::unexpected(failed.error());
                        }
                    }
                    co_return terminal;
                }
                auto response = responses.empty()
                    ? ai::assistant_text_message("default fake response")
                    : std::move(responses.front());
                if (!responses.empty()) {
                    responses.pop_front();
                }
                response.provider = "sdk-host";
                response.api = "fake";
                response.model = model.id;
                // Session files require real epoch timestamps on assistant
                // messages; stamp the deterministic value the other scripted
                // fakes use.
                response.timestamp = 1718000000123;
                if (sink) {
                    if (response.stop_reason == ai::AssistantStopReason::Error ||
                        response.stop_reason ==
                            ai::AssistantStopReason::Aborted) {
                        // Terminal-before-start: the loop synthesizes the
                        // assistant start from the authoritative final
                        // message.
                        if (auto failed = sink(ai::AssistantErrorEvent{
                                .reason = response.stop_reason,
                                .error = response,
                                .failure = support::make_error(
                                    support::ErrorCode::Stream,
                                    response.error_message.value_or(
                                        "terminal error")),
                            });
                            !failed) {
                            co_return std::unexpected(failed.error());
                        }
                    } else {
                        if (auto started =
                                sink(ai::AssistantStartEvent{response});
                            !started) {
                            co_return std::unexpected(started.error());
                        }
                    }
                }
                co_return response;
            });
    }

    void release_gate() {
        if (gate_) {
            (void)gate_->cancel();
        }
    }

    int request_count{0};
    /// When set, request number N blocks until released.
    std::optional<int> gate_request_number;
    std::vector<tests::RecordedProviderRequest> requests;
    std::deque<ai::AssistantMessage> responses;

private:
    std::optional<boost::asio::steady_timer> gate_;
};

[[nodiscard]] ai::Usage big_usage() {
    ai::Usage usage;
    usage.input = 5000;
    usage.output = 1000;
    usage.cache_read = 0;
    usage.cache_write = 0;
    usage.total_tokens = 6000;
    return usage;
}

[[nodiscard]] ai::AssistantMessage big_assistant(std::string text) {
    auto message = ai::assistant_text_message(std::move(text));
    message.usage = big_usage();
    return message;
}

/// One spawned manual compaction with a result slot.
using CompactResult =
    std::optional<support::Expected<coding_agent::CompactionResult>>;

void spawn_compact(
    boost::asio::io_context& io,
    coding_agent::AgentSession& session,
    CompactResult& slot) {
    boost::asio::co_spawn(
        io,
        session.compact(),
        [&slot](
            std::exception_ptr exception,
            support::Expected<coding_agent::CompactionResult> result) {
            REQUIRE(exception == nullptr);
            slot.emplace(std::move(result));
        });
}

} // namespace

TEST_CASE(
    "repeated Session Close is idempotent and rejects every work admission",
    "[coding_agent][runtime][close][issue467]") {
    CloseFixture fixture;
    auto options = fixture.options(tests::make_scripted_fake_provider());
    auto model_runtime = std::move(options.model_runtime);
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    auto created = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = std::move(model_runtime), .models = nullptr, .user_shell = nullptr}));
    REQUIRE(created.has_value());
    auto& session = fixture.runtime.adopt_session(std::move(created->session));

    session.close();
    // A repeated Close is a no-op: resources are released exactly once.
    session.close();
    CHECK_FALSE(session.is_open());
    CHECK_FALSE(session.is_busy());

    auto prompted = session.prompt_blocking("late prompt");
    REQUIRE_FALSE(prompted.has_value());
    CHECK(prompted.error().message == "session is closed");

    auto steered = session.steer("late steer");
    REQUIRE_FALSE(steered.has_value());
    CHECK(steered.error().message == "session is closed");

    auto followed = session.follow_up("late follow-up");
    REQUIRE_FALSE(followed.has_value());
    CHECK(followed.error().message == "session is closed");

    auto subscription = session.subscribe(
        [](const agent::AgentLifecycleEvent&) { return support::ExpectedVoid{}; });
    REQUIRE_FALSE(subscription.has_value());
    CHECK(subscription.error().message == "session is closed");

    auto compacted = run_awaitable(session.compact());
    REQUIRE_FALSE(compacted.has_value());
    CHECK(compacted.error().message == "session is closed");
}

TEST_CASE(
    "Session Close requested from an event subscriber finalizes after the run settles",
    "[coding_agent][runtime][close][issue467]") {
    CloseFixture fixture;
    auto options = fixture.options(tests::make_scripted_fake_provider());
    auto model_runtime = std::move(options.model_runtime);
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    auto created = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = std::move(model_runtime), .models = nullptr, .user_shell = nullptr}));
    REQUIRE(created.has_value());
    auto* session = &fixture.runtime.adopt_session(std::move(created->session));
    tests::RuntimeLoopDriver runtime_driver(fixture.runtime);

    bool closed_from_sink = false;
    auto subscribed = session->subscribe(
        [session, &closed_from_sink](const agent::AgentLifecycleEvent&)
            -> support::ExpectedVoid {
            if (!closed_from_sink) {
                closed_from_sink = true;
                // Close from inside an admitted callback is safe and
                // non-blocking (ADR 0011): finalization waits for this
                // callback and the run to quiesce.
                session->close();
            }
            return {};
        });
    REQUIRE(subscribed.has_value());
    auto subscription = std::move(*subscribed);

    auto prompted = session->prompt_blocking("close from subscriber");
    CHECK(prompted.has_value());
    CHECK(closed_from_sink);
    // The close finalized exactly once after the run settled.
    CHECK_FALSE(session->is_open());
    CHECK_FALSE(session->is_busy());
    // Finalization cleared the Agent subscriptions: the handle is inert.
    CHECK_FALSE(static_cast<bool>(subscription));
    // The admitted run's history persisted before teardown: the user
    // message and the aborted terminal assistant message (Close's
    // cancellation request resolved through pi's ordinary `aborted`
    // lifecycle, not a second error channel).
    auto loaded = harness::session::SessionStore::load(fixture.session_path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->messages.size() == 2);
    CHECK(
        ai::text_from_user_message(
            std::get<ai::UserMessage>(loaded->messages[0])) ==
        "close from subscriber");
    const auto& terminal =
        std::get<ai::AssistantMessage>(loaded->messages[1]);
    CHECK(terminal.stop_reason == ai::AssistantStopReason::Aborted);
}

TEST_CASE(
    "Session Close waits for an admitted compaction before releasing Session resources",
    "[coding_agent][runtime][close][issue467]") {
    CloseFixture fixture;
    const std::string big(20000, 'x');
    auto provider = std::make_shared<GatedScriptedProvider>();
    auto* gated = provider.get();
    gated->responses.push_back(big_assistant("a1 " + big));
    gated->responses.push_back(big_assistant("a2 " + big));
    gated->responses.push_back(big_assistant("a3 " + big));
    auto summary = ai::assistant_text_message("## Goal\nCompacted close summary");
    summary.usage = big_usage();
    gated->responses.push_back(summary);
    // The summarization request (after the three setup prompts) blocks until
    // the test releases it, so Close arrives mid-compaction.
    gated->gate_request_number = 4;

    auto options = fixture.options(std::move(provider));
    auto model_runtime = std::move(options.model_runtime);
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    auto created = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = std::move(model_runtime), .models = nullptr, .user_shell = nullptr}));
    REQUIRE(created.has_value());
    auto& session = fixture.runtime.adopt_session(std::move(created->session));
    tests::RuntimeLoopDriver runtime_driver(fixture.runtime);
    REQUIRE(session.prompt_blocking(big + " u1").has_value());
    REQUIRE(session.prompt_blocking(big + " u2").has_value());
    REQUIRE(session.prompt_blocking(big + " u3").has_value());
    REQUIRE(session.message_count() == 6);

    boost::asio::io_context io;
    CompactResult compact_result;
    spawn_compact(io, session, compact_result);
    drain_ready(io);
    REQUIRE(gated->request_count == 4);

    // Close stops admission immediately but must not release the Agent, the
    // store, or the persistence channel while the admitted compaction still
    // uses them.
    session.close();
    CHECK_FALSE(session.is_open());
    CHECK(session.is_busy());
    CHECK_FALSE(compact_result.has_value());

    auto rejected = session.prompt_blocking("during close");
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().message == "session is closed");

    gated->release_gate();
    REQUIRE(pump_until(io, [&] { return compact_result.has_value(); }));

    // The admitted compaction reached its terminal outcome, then Close
    // finalized exactly once.
    CHECK(compact_result->has_value());
    CHECK_FALSE(session.is_open());
    CHECK_FALSE(session.is_busy());

    // The CompactionEntry landed in the session file before the store was
    // released.
    auto loaded = harness::session::SessionStore::load(fixture.session_path);
    REQUIRE(loaded.has_value());
    bool found_compaction = false;
    for (const auto& entry : loaded->entries) {
        if (entry.kind == harness::session::SessionEntryKind::Compaction) {
            found_compaction = true;
            const auto& value =
                std::get<harness::session::CompactionEntryValue>(entry.value);
            CHECK(value.summary.find("Compacted close summary") !=
                  std::string::npos);
        }
    }
    CHECK(found_compaction);
}

TEST_CASE(
    "Session Close drains admitted Session Event Commitments under slow persistence",
    "[coding_agent][runtime][close][issue467]") {
    CloseFixture fixture;
    // Hold the model response on the ReleaseGate so Close observably arrives
    // while admitted work is in flight: without the gate, heavy parallel load
    // can starve this thread past the persistence delay and the whole run
    // settles before close(), collapsing the deferral assertions into a
    // wall-clock race (issue #526).
    auto provider = std::make_shared<tests::GatedChatProvider>();
    auto options = fixture.options(provider);
    auto model_runtime = std::move(options.model_runtime);
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    auto created = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = std::move(model_runtime), .models = nullptr, .user_shell = nullptr}));
    REQUIRE(created.has_value());
    auto& session = fixture.runtime.adopt_session(std::move(created->session));
    tests::RuntimeLoopDriver runtime_driver(fixture.runtime);

    harness::session::testing::delay_appends_for_test(
        fixture.session_path, std::chrono::milliseconds{300});

    boost::asio::io_context io;
    PromptResult prompt_result;
    spawn_prompt(io, session, "slow close", prompt_result);
    drain_ready(io);

    // The admitted run and its delayed persistence chain are both in flight;
    // Close defers finalization until every admitted Session Event
    // Commitment reaches its terminal outcome.
    session.close();
    CHECK(session.is_busy());

    provider->release();
    REQUIRE(pump_until(io, [&] { return prompt_result.has_value(); }));
    harness::session::testing::clear_append_delay_for_test(
        fixture.session_path);
    CHECK(prompt_result->has_value());
    CHECK_FALSE(session.is_open());
    CHECK_FALSE(session.is_busy());

    // Every admitted event persisted in order before the prompt settled.
    CHECK(fixture.persisted_texts() == std::vector<std::string>{"slow close", "turn 1"});
}

TEST_CASE(
    "subscriptions and late callbacks after Close are suppressed or benignly dropped",
    "[coding_agent][runtime][close][issue467]") {
    CloseFixture fixture;
    auto options = fixture.options(tests::make_scripted_fake_provider());
    auto model_runtime = std::move(options.model_runtime);
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    auto created = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = std::move(model_runtime), .models = nullptr, .user_shell = nullptr}));
    REQUIRE(created.has_value());
    auto& session = fixture.runtime.adopt_session(std::move(created->session));
    tests::RuntimeLoopDriver runtime_driver(fixture.runtime);

    std::size_t delivered = 0;
    auto subscribed = session.subscribe(
        [&delivered](const agent::AgentLifecycleEvent&) -> support::ExpectedVoid {
            ++delivered;
            return {};
        });
    REQUIRE(subscribed.has_value());
    auto subscription = std::move(*subscribed);
    auto session_subscribed = session.subscribe_session(
        [](const coding_agent::AgentSessionEvent&) -> support::ExpectedVoid {
            return {};
        });
    REQUIRE(session_subscribed.has_value());
    auto session_subscription = std::move(*session_subscribed);

    REQUIRE(session.prompt_blocking("observed run").has_value());
    CHECK(delivered > 0);

    session.close();
    // Finalization cleared the Agent subscribers: the handle reports inert
    // and no late delivery can reach the sink.
    CHECK_FALSE(static_cast<bool>(subscription));
    const auto delivered_at_close = delivered;

    // New subscriptions are rejected after Close.
    auto late = session.subscribe(
        [](const agent::AgentLifecycleEvent&) { return support::ExpectedVoid{}; });
    REQUIRE_FALSE(late.has_value());

    // Unsubscribing either handle after Close is a benign no-op.
    subscription.unsubscribe();
    session_subscription.unsubscribe();
    CHECK(delivered == delivered_at_close);
}

TEST_CASE(
    "Session Close during a retry backoff cancels the wait and settles the run",
    "[coding_agent][runtime][close][issue467]") {
    CloseFixture fixture;
    auto provider = std::make_shared<GatedScriptedProvider>();
    auto* scripted = provider.get();
    auto transient = ai::assistant_text_message("");
    transient.stop_reason = ai::AssistantStopReason::Error;
    transient.error_message = "overloaded_error";
    scripted->responses.push_back(std::move(transient));
    scripted->responses.push_back(ai::assistant_text_message("recovered"));

    // Keep an owning copy: Close releases the Session's provider after
    // quiescence, and the assertions below still observe the fake.
    auto options = fixture.options(provider);
    auto model_runtime = std::move(options.model_runtime);
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    auto created = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = std::move(model_runtime), .models = nullptr, .user_shell = nullptr}));
    REQUIRE(created.has_value());
    auto& session = fixture.runtime.adopt_session(std::move(created->session));
    tests::RuntimeLoopDriver runtime_driver(fixture.runtime);

    bool retry_started = false;
    bool retry_cancelled = false;
    auto session_subscribed = session.subscribe_session(
        [&](const coding_agent::AgentSessionEvent& event) -> support::ExpectedVoid {
            if (std::holds_alternative<coding_agent::AutoRetryStartEvent>(
                    event)) {
                retry_started = true;
            }
            if (const auto* ended =
                    std::get_if<coding_agent::AutoRetryEndEvent>(&event)) {
                if (ended->final_error == "Retry cancelled") {
                    retry_cancelled = true;
                }
            }
            return {};
        });
    REQUIRE(session_subscribed.has_value());
    auto session_subscription = std::move(*session_subscribed);

    boost::asio::io_context io;
    PromptResult prompt_result;
    spawn_prompt(io, session, "retry then close", prompt_result);
    // The first attempt failed retryably; the run now sits in the
    // abort-interruptible backoff sleep (pi `_prepareRetry`).
    REQUIRE(pump_until(io, [&] { return retry_started; }));

    // Close's cancellation request interrupts the backoff: the retry never
    // starts and the run settles through exactly one terminal outcome.
    session.close();
    REQUIRE(pump_until(io, [&] { return prompt_result.has_value(); }));
    CHECK(prompt_result->has_value());
    CHECK(retry_cancelled);
    CHECK(scripted->request_count == 1);
    CHECK_FALSE(session.is_open());
    CHECK_FALSE(session.is_busy());

    // The failed assistant message stayed in session history (pi
    // `_prepareRetry` removes it from live state only).
    const auto texts = fixture.persisted_texts();
    REQUIRE(texts.size() == 2);
    CHECK(texts.front() == "retry then close");
}

TEST_CASE(
    "Session Close with a latched persistence failure completes and keeps the session file consistent",
    "[coding_agent][runtime][close][issue467]") {
    CloseFixture fixture;
    // Hold the model response on the ReleaseGate so Close observably arrives
    // while admitted work is in flight: without the gate, heavy parallel load
    // can starve this thread past the persistence delay and the run settles
    // before close(), making the is_busy assertion a wall-clock race
    // (issue #526).
    auto provider = std::make_shared<tests::GatedChatProvider>();
    auto options = fixture.options(provider);
    auto model_runtime = std::move(options.model_runtime);
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    auto created = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = std::move(model_runtime), .models = nullptr, .user_shell = nullptr}));
    REQUIRE(created.has_value());
    auto& session = fixture.runtime.adopt_session(std::move(created->session));
    tests::RuntimeLoopDriver runtime_driver(fixture.runtime);

    // The user message lands; the assistant append fails slowly, so the
    // failing persistence channel is still ahead of the run when Close
    // requests quiescence.
    harness::session::testing::fail_nth_append_for_test(fixture.session_path, 2);
    harness::session::testing::delay_appends_for_test(
        fixture.session_path, std::chrono::milliseconds{300});

    boost::asio::io_context io;
    PromptResult prompt_result;
    spawn_prompt(io, session, "failing close", prompt_result);
    drain_ready(io);

    session.close();
    CHECK(session.is_busy());

    provider->release();
    REQUIRE(pump_until(io, [&] { return prompt_result.has_value(); }));
    harness::session::testing::clear_append_delay_for_test(
        fixture.session_path);

    // The admitted run settles with the typed persistence failure.
    REQUIRE_FALSE(prompt_result->has_value());
    CHECK(prompt_result->error().code == support::ErrorCode::Session);
    CHECK(prompt_result->error().message == "could not persist session entry");

    // Close still finalizes: the failed channel drains fail-fast instead of
    // hanging finalization.
    CHECK_FALSE(session.is_open());
    CHECK_FALSE(session.is_busy());

    // No rollback theatre: the file keeps the admitted user message, and
    // post-Close prompts reject with the closed-session error.
    const auto rejected = session.prompt_blocking("after close");
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().message == "session is closed");
    CHECK(
        fixture.persisted_texts() == std::vector<std::string>{"failing close"});
}
