// Session Assembly contract suite (#509, map #508): pins the Agent Session
// creation boundary's contract before any reshape. The boundary is
// `create_agent_session` (the one door over the private SessionFactory); the
// suite crosses only that door and asserts the four grilled invariants:
//
//   1. Settings snapshot semantics — project settings defaults select the
//      model only while the project is trusted (the single-snapshot rule's
//      observable consequence; literal load counting would need
//      instrumentation the Owner does not expose).
//   2. No stdio writes — creation reports through returned values
//      (diagnostics/errors), never through stdout/stderr.
//   3. Request Authentication precedence — runtime override > stored
//      credential > configured key survives assembly unchanged.
//   4. Diagnostics ordering — settings load errors surface ahead of resource
//      diagnostics (pi report order).
//
// No live keys or network: providers resolve from a temp Agent Config
// Directory (`PI_CODING_AGENT_DIR`) with env-template keys guarded by
// `EnvVarGuard`.

#include <cch/ai/Models.hpp>
#include <cch/coding_agent/AuthStorage.hpp>
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/AgentSessionCreationRequest.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ModelsFixture.hpp"
#include "support/StreamAdapterFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace cch;
namespace runtime = cch::coding_agent::runtime;

namespace {

/// One isolated assembly fixture: temp workspace + temp Agent Config
/// Directory. Ambient provider keys are guarded so built-in providers never
/// resolve as configured unless a test says so.
struct AssemblyFixture {
    tests::TempWorkspace workspace;
    tests::TempWorkspace agent_dir;
    tests::EnvVarGuard dir_guard{"PI_CODING_AGENT_DIR"};
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard alpha_guard{"ALPHA_KEY"};
    tests::EnvVarGuard beta_guard{"BETA_KEY"};
    tests::EnvVarGuard gamma_guard{"GAMMA_KEY"};
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};
    tests::EnvVarGuard deepseek_guard{"DEEPSEEK_API_KEY"};

    AssemblyFixture() {
        dir_guard.set(agent_dir.path().string());
        home_guard.set(workspace.path().string());
        kimi_guard.unset();
        deepseek_guard.unset();
    }

    void write_models(std::string_view json) {
        std::ofstream out(agent_dir.path() / "models.json", std::ios::binary);
        out << json;
    }

    [[nodiscard]] runtime::AgentSessionCreationRequest make_request(
        std::optional<bool> trust_override = std::nullopt) const {
        runtime::AgentSessionCreationRequest request;
        request.execution_runtime_target = tests::detail::fixture_runtime_target();
        request.project_trust_override = trust_override;
        request.session_facts.no_skills = true;
        request.session_facts.no_prompt_templates = true;
        request.workspace = workspace.path();
        request.session_target = coding_agent::InMemorySessionTarget{};
        return request;
    }

    [[nodiscard]] support::Expected<coding_agent::CreateAgentSessionResult> create(
        runtime::AgentSessionCreationRequest request) const {
        return coding_agent::create_agent_session(std::move(request));
    }
};

constexpr std::string_view kTwoProviderModels = R"({
  "providers": {
    "alpha": {
      "baseUrl": "https://alpha.example/v1",
      "api": "openai-responses",
      "apiKey": "$ALPHA_KEY",
      "models": [{"id": "alpha-1"}]
    },
    "beta": {
      "baseUrl": "https://beta.example/v1",
      "api": "openai-responses",
      "apiKey": "$BETA_KEY",
      "models": [{"id": "beta-2"}]
    }
  }
})";

/// Captures everything written to one fd between construction and restore().
/// POSIX dup2 redirection into a tmpfile; restored before assertions so
/// Catch2's own output is untouched.
class StreamCapture {
public:
    explicit StreamCapture(int fd)
        : fd_(fd), original_(::dup(fd)), sink_(::tmpfile()) {
        std::fflush(nullptr);
        ::dup2(::fileno(sink_), fd_);
    }

    StreamCapture(const StreamCapture&) = delete;
    StreamCapture& operator=(const StreamCapture&) = delete;

    ~StreamCapture() { restore(); }

    void restore() {
        if (!restored_) {
            std::fflush(nullptr);
            ::dup2(original_, fd_);
            ::close(original_);
            restored_ = true;
        }
    }

    [[nodiscard]] std::string content() {
        restore();
        std::fflush(nullptr);
        std::rewind(sink_);
        std::string data;
        char buffer[512];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), sink_)) > 0) {
            data.append(buffer, read);
        }
        return data;
    }

private:
    int fd_;
    int original_;
    std::FILE* sink_;
    bool restored_{false};
};

} // namespace

TEST_CASE(
    "session creation writes nothing to stdout or stderr on the success path",
    "[coding_agent][runtime][assembly-contract][issue509]") {
    AssemblyFixture fix;
    fix.write_models(kTwoProviderModels);
    fix.alpha_guard.set("alpha-key");
    fix.beta_guard.set("beta-key");

    StreamCapture capture_out{STDOUT_FILENO};
    StreamCapture capture_err{STDERR_FILENO};
    const auto created = fix.create(fix.make_request());
    capture_out.restore();
    capture_err.restore();

    REQUIRE(created);
    CHECK(capture_out.content().empty());
    CHECK(capture_err.content().empty());
}

TEST_CASE(
    "session creation reports failure through the error channel, not stdio",
    "[coding_agent][runtime][assembly-contract][issue509]") {
    AssemblyFixture fix;
    fix.write_models(kTwoProviderModels);
    fix.alpha_guard.set("alpha-key");
    fix.beta_guard.set("beta-key");

    // A non-regular session target entry is refused by assembly.
    const auto directory_target = fix.workspace.path() / "target-directory";
    std::filesystem::create_directories(directory_target);

    auto request = fix.make_request();
    request.session_target =
        coding_agent::ExplicitOpenOrCreateSessionTarget{.path = directory_target};

    StreamCapture capture_out{STDOUT_FILENO};
    StreamCapture capture_err{STDERR_FILENO};
    const auto created = fix.create(std::move(request));
    capture_out.restore();
    capture_err.restore();

    REQUIRE_FALSE(created);
    CHECK(capture_out.content().empty());
    CHECK(capture_err.content().empty());
}

TEST_CASE(
    "project settings defaults select the model only while the project is trusted",
    "[coding_agent][runtime][assembly-contract][issue509]") {
    AssemblyFixture fix;
    fix.write_models(kTwoProviderModels);
    fix.alpha_guard.set("alpha-key");
    fix.beta_guard.set("beta-key");
    fix.workspace.write(
        ".pi/settings.json",
        R"({"defaultProvider": "beta", "defaultModel": "beta-2"})");

    // Trusted: the project settings default wins over first-available.
    auto trusted = fix.create(fix.make_request(/*trust_override=*/true));
    REQUIRE(trusted);
    CHECK(trusted->resolved_identity.provider == "beta");
    CHECK(trusted->resolved_identity.model == "beta-2");

    // Untrusted: the project scope never loads, so the project default is
    // invisible to the resolution chain.
    auto untrusted = fix.create(fix.make_request(/*trust_override=*/false));
    REQUIRE(untrusted);
    CHECK(untrusted->resolved_identity.provider != "beta");
    CHECK(untrusted->resolved_identity.model != "beta-2");
}

TEST_CASE(
    "Request Authentication precedence survives assembly unchanged",
    "[coding_agent][runtime][assembly-contract][issue509]") {
    AssemblyFixture fix;
    fix.write_models(R"({
      "providers": {
        "gamma": {
          "baseUrl": "https://gamma.example/v1",
          "api": "openai-responses",
          "apiKey": "$GAMMA_KEY",
          "models": [{"id": "gamma-1"}]
        }
      }
    })");
    fix.gamma_guard.set("env-configured-key");

    // Level 4 (configured key): ambient env-template key resolves when
    // nothing is stored and no override exists.
    auto ambient = fix.create(fix.make_request());
    REQUIRE(ambient);
    {
        auto auth = tests::run_awaitable(
            ambient->session->model_runtime()->get_auth("gamma"));
        REQUIRE(auth);
        REQUIRE(*auth);
        CHECK((*auth)->auth.api_key == "env-configured-key");
        CHECK((*auth)->source == "configured API key");
    }

    // Level 2 (stored credential) beats the configured key.
    coding_agent::AuthStorage storage(fix.agent_dir.path() / "auth.json");
    auto stored = tests::run_async_result(storage.modify(
        "gamma",
        [](std::optional<ai::Credential>)
            -> support::AsyncResult<std::optional<ai::Credential>> {
            return support::AsyncResult<std::optional<ai::Credential>>(
                std::expected<std::optional<ai::Credential>, support::Error>{
                    std::optional<ai::Credential>{
                        ai::ApiKeyCredential{.key = "stored-key"}}});
        }));
    REQUIRE(stored);

    auto with_stored = fix.create(fix.make_request());
    REQUIRE(with_stored);
    {
        auto auth = tests::run_awaitable(
            with_stored->session->model_runtime()->get_auth("gamma"));
        REQUIRE(auth);
        REQUIRE(*auth);
        CHECK((*auth)->auth.api_key == "stored-key");
    }

    // Level 1 (runtime API key override from --api-key) beats everything and
    // installs as the process-lifetime in-memory override.
    auto override_request = fix.make_request();
    override_request.session_facts.provider = "gamma";
    override_request.session_facts.model = "gamma-1";
    override_request.session_facts.api_key = "cli-override-key";
    auto overridden = fix.create(std::move(override_request));
    REQUIRE(overridden);
    CHECK(overridden->session->model_runtime()->has_runtime_api_key("gamma"));
    {
        auto auth = tests::run_awaitable(
            overridden->session->model_runtime()->get_auth("gamma"));
        REQUIRE(auth);
        REQUIRE(*auth);
        CHECK((*auth)->auth.api_key == "cli-override-key");
    }
}

TEST_CASE(
    "settings load errors surface as diagnostics ahead of resource diagnostics",
    "[coding_agent][runtime][assembly-contract][issue509]") {
    AssemblyFixture fix;
    fix.write_models(kTwoProviderModels);
    fix.alpha_guard.set("alpha-key");
    fix.beta_guard.set("beta-key");

    // Broken global settings plus a duplicate project skill name: both kinds
    // of diagnostic fire in one creation attempt.
    fix.agent_dir.write("settings.json", "{ this is not json");
    fix.workspace.write(
        ".pi/skills/first/SKILL.md",
        "---\nname: dupe-skill\ndescription: First skill.\n---\nFirst body.\n");
    fix.workspace.write(
        ".pi/skills/second/SKILL.md",
        "---\nname: dupe-skill\ndescription: Duplicate skill.\n---\nDuplicate body.\n");

    auto request = fix.make_request(/*trust_override=*/true);
    request.session_facts.no_skills = false;
    const auto created = fix.create(std::move(request));
    REQUIRE(created);
    const auto& diagnostics = created->diagnostics;
    REQUIRE_FALSE(diagnostics.empty());

    const auto settings_index = std::find_if(
        diagnostics.begin(), diagnostics.end(), [](const auto& diag) {
            return diag.code.rfind("settings:", 0) == 0;
        });
    const auto resource_index = std::find_if(
        diagnostics.begin(), diagnostics.end(), [](const auto& diag) {
            return diag.code.rfind("duplicate:", 0) == 0 ||
                diag.code.rfind("resource:", 0) == 0;
        });
    REQUIRE(settings_index != diagnostics.end());
    REQUIRE(resource_index != diagnostics.end());
    // pi report order: settings errors precede resource diagnostics.
    CHECK(settings_index < resource_index);
}
