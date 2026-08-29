// Reuse one Runtime root across Session replacement (issue #466, criteria
// 3–4): the interactive host builds one ModelRuntime and shares it with the
// boot Session and every in-session replacement, so the Runtime's Models
// resources are reused rather than reconstructed for each Session, and
// closing a Session does not release the shared runtime the replacement
// Session needs (ADR 0029/0030, ADR 0040).

#include "support/EnvVarGuard.hpp"
#include "support/ScriptedRuntimeFixture.hpp"
#include "support/ModelsFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/agent/harness/session/SessionStore.hpp>
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

using namespace cch;

namespace {

struct Fixture {
    tests::TempWorkspace workspace;
    tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR"};

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
            const std::filesystem::path& path, const std::shared_ptr<coding_agent::ModelRuntime>& runtime) const {
        coding_agent::runtime::AgentSessionCreationRequest request;
        request.workspace = workspace.path();
        request.session_target =
            coding_agent::ExplicitResumeSessionTarget{path};
        request.session_facts.no_skills = true;
        request.session_facts.no_prompt_templates = true;
        request.execution_runtime_target = tests::detail::fixture_runtime_target();
        request.model_runtime = runtime;
        return request;
    }
};

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

    // Session A shares the host runtime through the production single-argument
    // create path, exactly what the interactive CLI host calls.
    auto created_a = coding_agent::create_agent_session(
        fixture.request(path, runtime));
    REQUIRE(created_a);
    CHECK(created_a->session->model() == "fake-model");
    REQUIRE(created_a->session->prompt_blocking("first on shared runtime").has_value());
    REQUIRE(scripted.control->calls.size() == 1);

    // Closing Session A must not release the host-shared Models runtime
    // (criterion 4): the replacement Session still needs it. The Session
    // never owns a host-injected runtime.
    created_a->session->close();
    CHECK(created_a->session->model_runtime() == runtime);

    // Session B (the replacement) reuses the same runtime instead of
    // reconstructing one (criterion 3).
    auto created_b = coding_agent::create_agent_session(
        fixture.request(path, runtime));
    REQUIRE(created_b);
    REQUIRE(created_b->session->prompt_blocking("second on shared runtime").has_value());
    REQUIRE(scripted.control->calls.size() == 2);
    CHECK(created_b->session->model_runtime() == runtime);

    created_b->session->close();
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
    owned_request.execution_runtime_target = tests::detail::fixture_runtime_target();
    auto created_owned = coding_agent::create_agent_session_for_testing(
            std::move(owned_request), tests::make_scripted_fake_models());
    REQUIRE(created_owned);
    REQUIRE(created_owned->session->model_runtime() != nullptr);
    created_owned->session->close();
    CHECK(created_owned->session->model_runtime() == nullptr);
}
