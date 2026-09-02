// Reuse one Runtime root across Session replacement (issue #466, criteria
// 3–4): the interactive host builds one ModelRuntime and shares it with the
// boot Session and every in-session replacement, so the Runtime's Models
// resources are reused rather than reconstructed for each Session, and
// closing a Session does not release the shared runtime the replacement
// Session needs (ADR 0029/0030, ADR 0040).

#include "support/AsyncResultBridge.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/ScriptedRuntimeFixture.hpp"
#include "support/ModelsFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/agent/harness/session/SessionStore.hpp>
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>
#include <boost/asio/awaitable.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using namespace cch;

namespace {

struct Fixture {
    tests::TempWorkspace workspace;
    tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};
    tests::RuntimeFixture runtime;

    Fixture() { agent_dir_guard.set(agent_dir.path().string()); }

    /// A persisted session file carrying the `fake/fake-model` identity, so
    /// the resume chain resolves the model through the shared runtime (a
    /// fresh in-memory session would consult the runtime's availability
    /// snapshot, which a host-injected concrete runtime still populates
    /// through its registered scripted Providers).
    [[nodiscard]] std::filesystem::path session_file() const {
        const auto path = workspace.path() / "shared-runtime.jsonl";
        auto store = harness::session::SessionStore::create_new(
            path,
            {
                .session_id = "shared-runtime",
                .created_at = "2026-08-16T00:00:00Z",
                .workspace = workspace.path(),
                .provider = "fake",
                .model = "fake-model",
            });
        REQUIRE(store);
        REQUIRE(store->append(ai::MessageVariant{ai::user_text_message(
            "resume seed", 1'700'000'000'000)}));
        ai::AssistantMessage assistant;
        assistant.provider = "fake";
        assistant.api = "fake";
        assistant.model = "fake-model";
        assistant.stop_reason = ai::AssistantStopReason::Stop;
        assistant.timestamp = 1'700'000'000'001;
        assistant.content.emplace_back(
            ai::text_content("resumed seed reply"));
        REQUIRE(store->append(ai::MessageVariant{assistant}));
        return path;
    }

    /// One Session creation request sharing the host runtime (the field the
    /// production interactive CLI host sets on the boot and every in-session
    /// replacement request).
    [[nodiscard]] coding_agent::runtime::AgentSessionCreationRequest request(
            const std::filesystem::path& path, const std::shared_ptr<coding_agent::ModelRuntime>& model_runtime) const {
        coding_agent::runtime::AgentSessionCreationRequest request;
        request.workspace = workspace.path();
        request.session_target =
            coding_agent::ExplicitResumeSessionTarget{path};
        request.session_facts.no_skills = true;
        request.session_facts.no_prompt_templates = true;
        request.execution_runtime_target = runtime.make_target();
        request.model_runtime = model_runtime;
        return request;
    }
};

/// The Session must outlive `runtime_fixture.run`; the one-shot factory
/// captures its reference while the prompt settles on the fixture loop.
[[nodiscard]] support::ExpectedVoid run_prompt(
        tests::RuntimeFixture& runtime_fixture, coding_agent::AgentSession& session, std::string text) {
    return runtime_fixture.run(support::detail::make_async_result(
            [&session, text = std::move(text)]() -> boost::asio::awaitable<support::ExpectedVoid> {
                co_return co_await session.prompt(std::move(text));
            }));
}

} // namespace

TEST_CASE(
    "a host-shared ModelRuntime survives Session close and is reused by the replacement",
    "[coding_agent][runtime][reuse][issue466]") {
    Fixture fixture;
    const auto path = fixture.session_file();
    tests::ScriptedRuntimeFixture scripted;
    auto runtime = scripted.runtime;
    // Session A's response carries the fake/fake-model identity so the shared
    // session file keeps a resumable model for Session B.
    auto first_reply = ai::assistant_text_message(
        "first done", 1'700'000'000'002);
    first_reply.provider = "fake";
    first_reply.api = "fake";
    first_reply.model = "fake-model";
    scripted.control->responses.push_back(std::move(first_reply));

    // Session A shares the host runtime through the asynchronous production
    // Session Assembly door, exactly what the interactive CLI host calls.
    auto created_a = fixture.runtime.run(
            coding_agent::create_agent_session_async(fixture.request(path, runtime), std::nullopt, {}));
    REQUIRE(created_a);
    auto& session_a = fixture.runtime.adopt_session(std::move(created_a->session));
    CHECK(session_a.model() == "fake-model");
    REQUIRE(run_prompt(fixture.runtime, session_a, "first on shared runtime").has_value());
    REQUIRE(scripted.control->calls.size() == 1);

    // Closing Session A must not release the host-shared Models runtime
    // (criterion 4): the replacement Session still needs it. The Session
    // never owns a host-injected runtime.
    session_a.close();
    CHECK(session_a.model_runtime() == runtime);

    // Session B (the replacement) reuses the same runtime instead of
    // reconstructing one (criterion 3).
    auto created_b = fixture.runtime.run(
            coding_agent::create_agent_session_async(fixture.request(path, runtime), std::nullopt, {}));
    REQUIRE(created_b);
    auto& session_b = fixture.runtime.adopt_session(std::move(created_b->session));
    REQUIRE(run_prompt(fixture.runtime, session_b, "second on shared runtime").has_value());
    REQUIRE(scripted.control->calls.size() == 2);
    CHECK(session_b.model_runtime() == runtime);

    session_b.close();
}

TEST_CASE(
    "a Session-created runtime is released on close while a host-shared runtime is retained",
    "[coding_agent][runtime][reuse][issue466]") {
    Fixture fixture;
    const auto path = fixture.session_file();

    // Default-created (Session-owned) runtime: close releases it.
    coding_agent::runtime::AgentSessionCreationRequest owned_request;
    owned_request.workspace = fixture.workspace.path();
    owned_request.session_target =
        coding_agent::ExplicitResumeSessionTarget{path};
    owned_request.session_facts.no_skills = true;
    owned_request.session_facts.no_prompt_templates = true;
    owned_request.execution_runtime_target = fixture.runtime.make_target();
    auto created_owned = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(owned_request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = nullptr, .models = tests::make_scripted_fake_models(), .user_shell = nullptr}));
    REQUIRE(created_owned);
    auto& owned_session = fixture.runtime.adopt_session(std::move(created_owned->session));
    REQUIRE(owned_session.model_runtime() != nullptr);
    owned_session.close();
    CHECK(owned_session.model_runtime() == nullptr);
}
