#include "coding_agent/AgentSession.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/ScriptedRuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "support/UniqueFd.hpp"

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>
#include <boost/asio/post.hpp>

#include <chrono>
#include <csignal>
#include <expected>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace cch;

namespace {

template <typename E>
[[nodiscard]] support::AsyncResult<int, E> post_result(
        const std::shared_ptr<harness::RuntimeTarget>& target, std::expected<int, E> result) {
    return support::AsyncResult<int, E>{support::AsyncProducer<int, E>{
            [target, result = std::move(result)](support::AsyncCompletion<int, E> completion) mutable noexcept {
                const auto completion_owner = std::make_shared<support::AsyncCompletion<int, E>>(std::move(completion));
                const auto result_owner = std::make_shared<std::expected<int, E>>(std::move(result));
                boost::asio::post(target->executor(), [completion_owner, result_owner]() mutable noexcept {
                    std::move (*completion_owner)(std::move(*result_owner));
                });
            }}};
}

bool timeout_child_fails() {
    const pid_t pid = ::fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        (void)::signal(SIGABRT, SIG_DFL);
        support::UniqueFd devnull(::open("/dev/null", O_WRONLY));
        if (devnull) {
            (void)::dup2(devnull.get(), STDOUT_FILENO);
            (void)::dup2(devnull.get(), STDERR_FILENO);
        }

        tests::RuntimeFixture runtime(harness::RuntimeLimits{}, std::chrono::milliseconds{25});
        support::AsyncResult<int> stalled{support::AsyncProducer<int, support::Error>{
                [](support::AsyncCompletion<int, support::Error>) noexcept {}}};
        (void)runtime.run(std::move(stalled));
        ::_exit(0);
    }

    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            return WIFEXITED(status) && WEXITSTATUS(status) != 0;
        }
        if (waited < 0) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    (void)::kill(pid, SIGKILL);
    (void)::waitpid(pid, &status, 0);
    return false;
}

class RecordingSession final {
public:
    explicit RecordingSession(std::vector<std::string>& events) : events_(&events) {}

    ~RecordingSession() { events_->push_back("session-destroyed"); }

    void close() noexcept {
        events_->push_back("session-close");
        open_ = false;
    }

    [[nodiscard]] bool is_open() const noexcept { return open_; }
    [[nodiscard]] bool is_busy() const noexcept { return false; }

private:
    std::vector<std::string>* events_; // must outlive this borrowed Session.
    bool open_{true};
};

struct DeferredSessionState final {
    std::atomic<bool> close_requested{false};
    std::atomic<bool> released{false};
    std::atomic<bool> destroyed{false};
};

class DeferredSession final {
public:
    explicit DeferredSession(std::shared_ptr<DeferredSessionState> state) : state_(std::move(state)) {}

    ~DeferredSession() { state_->destroyed.store(true, std::memory_order_release); }

    void close() noexcept { state_->close_requested.store(true, std::memory_order_release); }

    [[nodiscard]] bool is_open() const noexcept { return !state_->close_requested.load(std::memory_order_acquire); }

    [[nodiscard]] bool is_busy() const noexcept { return !state_->released.load(std::memory_order_acquire); }

private:
    std::shared_ptr<DeferredSessionState> state_;
};

struct SharedSessionFixture final {
    tests::TempWorkspace workspace;
    tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    tests::EnvVarGuard home_guard{"HOME"};
    tests::ScriptedRuntimeFixture scripted;
    tests::RuntimeFixture runtime;
    std::shared_ptr<harness::RuntimeTarget> target;

    SharedSessionFixture() : target(runtime.make_target()) {
        agent_dir_guard.set(agent_dir.path().string());
        home_guard.set(workspace.path().string());
    }

    [[nodiscard]] coding_agent::runtime::AgentSessionCreationRequest request() const {
        coding_agent::runtime::AgentSessionCreationRequest request;
        request.execution_runtime_target = target;
        request.model_runtime = scripted.runtime;
        request.request_model = tests::scripted_request_model("fake", "fake-model");
        request.session_facts.no_skills = true;
        request.session_facts.no_prompt_templates = true;
        request.session_target = coding_agent::InMemorySessionTarget{};
        request.workspace = workspace.path();
        return request;
    }
};

} // namespace

TEST_CASE("RuntimeFixture preserves ready AsyncResult success and error outcomes",
        "[coding_agent][runtime][harness][issue568]") {
    tests::RuntimeFixture runtime;

    const auto success = runtime.run(support::AsyncResult<int>{std::expected<int, support::Error>{42}});
    REQUIRE(success);
    CHECK(*success == 42);

    const auto error = runtime.run(support::AsyncResult<int>{std::expected<int, support::Error>{
            std::unexpect, support::make_error(support::ErrorCode::Process, "ready failure")}});
    REQUIRE_FALSE(error);
    CHECK(error.error().code == support::ErrorCode::Process);
    CHECK(error.error().message == "ready failure");
}

TEST_CASE("RuntimeFixture preserves pending AsyncResult success and error outcomes",
        "[coding_agent][runtime][harness][issue568]") {
    tests::RuntimeFixture runtime;
    const auto target = runtime.make_target();

    const auto success = runtime.run(post_result(target, std::expected<int, support::Error>{23}));
    REQUIRE(success);
    CHECK(*success == 23);

    const auto error = runtime.run(post_result(target,
            std::expected<int, support::Error>{
                    std::unexpect, support::make_error(support::ErrorCode::Cancelled, "pending cancellation")}));
    REQUIRE_FALSE(error);
    CHECK(error.error().code == support::ErrorCode::Cancelled);
    CHECK(error.error().message == "pending cancellation");
}

TEST_CASE("RuntimeFixture bounds a stalled AsyncResult as a test failure",
        "[coding_agent][runtime][harness][fatal][issue568]") {
    const auto started = std::chrono::steady_clock::now();
    CHECK(timeout_child_fails());
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds{1});
}

TEST_CASE("RuntimeFixture instances keep Runtime targets isolated", "[coding_agent][runtime][harness][issue568]") {
    tests::RuntimeFixture first;
    tests::RuntimeFixture second;

    const auto first_result = first.run(post_result(first.make_target(), std::expected<int, support::Error>{1}));
    const auto second_result = second.run(post_result(second.make_target(), std::expected<int, support::Error>{2}));

    REQUIRE(first_result);
    REQUIRE(second_result);
    CHECK(*first_result == 1);
    CHECK(*second_result == 2);
}

TEST_CASE("multiple Agent Sessions share one RuntimeFixture and close before RuntimeRoot",
        "[coding_agent][runtime][harness][issue568]") {
    SharedSessionFixture fixture;

    auto first_result =
            fixture.runtime.run(coding_agent::create_agent_session_async(fixture.request(), std::nullopt, {}));
    REQUIRE(first_result);
    REQUIRE(first_result->session);
    auto& first = fixture.runtime.adopt_session(std::move(first_result->session));

    auto second_result =
            fixture.runtime.run(coding_agent::create_agent_session_async(fixture.request(), std::nullopt, {}));
    REQUIRE(second_result);
    REQUIRE(second_result->session);
    auto& second = fixture.runtime.adopt_session(std::move(second_result->session));

    CHECK(first.is_open());
    CHECK(second.is_open());
    CHECK(first.model_runtime() == fixture.scripted.runtime);
    CHECK(second.model_runtime() == fixture.scripted.runtime);

    fixture.runtime.close();
    CHECK(fixture.runtime.closed());
    REQUIRE(fixture.runtime.teardown_events().size() == 6);
    CHECK(fixture.runtime.teardown_events()[0] == tests::RuntimeTeardownEvent::SessionCloseRequested);
    CHECK(fixture.runtime.teardown_events()[1] == tests::RuntimeTeardownEvent::SessionsQuiesced);
    CHECK(fixture.runtime.teardown_events()[2] == tests::RuntimeTeardownEvent::SessionsDestroyed);
    CHECK(fixture.runtime.teardown_events()[3] == tests::RuntimeTeardownEvent::RuntimeClose);
    CHECK(fixture.runtime.teardown_events()[4] == tests::RuntimeTeardownEvent::RuntimeWorkersJoined);
    CHECK(fixture.runtime.teardown_events()[5] == tests::RuntimeTeardownEvent::RuntimeLoopDrained);
}

TEST_CASE("RuntimeFixture waits for admitted Session work before RuntimeRoot close",
        "[coding_agent][runtime][harness][issue568]") {
    tests::RuntimeFixture runtime;
    const auto target = runtime.make_target();
    const auto state = std::make_shared<DeferredSessionState>();
    (void)runtime.adopt_session(std::make_unique<DeferredSession>(state));

    std::thread releaser([state, target] {
        while (!state->close_requested.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        boost::asio::post(
                target->executor(), [state]() noexcept { state->released.store(true, std::memory_order_release); });
    });

    runtime.close();
    releaser.join();

    CHECK(state->released.load(std::memory_order_acquire));
    CHECK(state->destroyed.load(std::memory_order_acquire));
}

TEST_CASE("RuntimeFixture records Session destruction before Runtime worker join",
        "[coding_agent][runtime][harness][issue568]") {
    std::vector<std::string> session_events;
    tests::RuntimeFixture runtime;
    (void)runtime.adopt_session(std::make_unique<RecordingSession>(session_events));

    runtime.close();

    CHECK(session_events == std::vector<std::string>{"session-close", "session-destroyed"});
    const auto& teardown = runtime.teardown_events();
    REQUIRE(teardown.size() == 6);
    CHECK(teardown[0] == tests::RuntimeTeardownEvent::SessionCloseRequested);
    CHECK(teardown[1] == tests::RuntimeTeardownEvent::SessionsQuiesced);
    CHECK(teardown[2] == tests::RuntimeTeardownEvent::SessionsDestroyed);
    CHECK(teardown[3] == tests::RuntimeTeardownEvent::RuntimeClose);
    CHECK(teardown[4] == tests::RuntimeTeardownEvent::RuntimeWorkersJoined);
    CHECK(teardown[5] == tests::RuntimeTeardownEvent::RuntimeLoopDrained);
}
