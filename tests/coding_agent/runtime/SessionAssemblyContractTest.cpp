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
// Directory (`HOME`) with env-template keys guarded by
// `EnvVarGuard`.

#include <cch/ai/Models.hpp>
#include "ai/ModelsTestAccess.hpp"
#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/coding_agent/Skill.hpp>
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/AgentSessionAssembly.hpp"
#include "coding_agent/runtime/AgentSessionCreationRequest.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/ModelCatalogFixture.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/StreamAdapterFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <thread>
#include <vector>
#include "support/AgentRootFixture.hpp"
#include "coding_agent/ModelRuntimeTestSupport.hpp"

using namespace cch;
namespace runtime = cch::coding_agent::runtime;

namespace {

/// One isolated assembly fixture: temp workspace + temp Agent Config
/// Directory. Ambient provider keys are guarded so built-in providers never
/// resolve as configured unless a test says so.
struct AssemblyFixture {
    tests::TempWorkspace workspace;
    std::filesystem::path agent_dir;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard alpha_guard{"ALPHA_KEY"};
    tests::EnvVarGuard beta_guard{"BETA_KEY"};
    tests::EnvVarGuard gamma_guard{"GAMMA_KEY"};
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};
    tests::EnvVarGuard deepseek_guard{"DEEPSEEK_API_KEY"};
    tests::ModelCatalogFixture catalog;
    tests::RuntimeFixture runtime;

    AssemblyFixture() {
        home_guard.set(workspace.path().string());
        agent_dir = tests::agent_root_under_home(workspace.path());
        std::filesystem::create_directories(agent_dir);
        kimi_guard.unset();
        deepseek_guard.unset();
    }

    void write_models(std::string_view json) { catalog.set(json); }

    [[nodiscard]] runtime::AgentSessionCreationRequest make_request(
        std::optional<bool> trust_override = std::nullopt) const {
        runtime::AgentSessionCreationRequest request;
        request.execution_runtime_target = runtime.make_target();
        request.project_trust_override = trust_override;
        request.session_facts.no_skills = true;
        request.session_facts.no_prompt_templates = true;
        request.workspace = workspace.path();
        request.model_runtime = catalog.runtime;
        request.session_target = coding_agent::InMemorySessionTarget{};
        return request;
    }

    [[nodiscard]] support::Expected<coding_agent::CreateAgentSessionResult> create(
            runtime::AgentSessionCreationRequest request) {
        return runtime.run(coding_agent::create_agent_session_async(std::move(request), std::nullopt, {}));
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

TEST_CASE("session creation writes nothing to stdout or stderr on the success path",
        "[coding_agent][runtime][assembly-contract][issue509][spec]") {
    AssemblyFixture fix;
    fix.write_models(kTwoProviderModels);
    fix.alpha_guard.set("alpha-key");
    fix.beta_guard.set("beta-key");

    StreamCapture capture_out{STDOUT_FILENO};
    StreamCapture capture_err{STDERR_FILENO};
    auto created = fix.create(fix.make_request());
    capture_out.restore();
    capture_err.restore();

    REQUIRE(created);
    (void)fix.runtime.adopt_session(std::move(created->session));
    CHECK(capture_out.content().empty());
    CHECK(capture_err.content().empty());
}

TEST_CASE("session creation reports failure through the error channel, not stdio",
        "[coding_agent][runtime][assembly-contract][issue509][spec]") {
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

TEST_CASE("session creation persists nothing on disk before the first user or assistant message",
        "[coding_agent][runtime][assembly-contract][spec]") {
    AssemblyFixture fix;
    fix.write_models(kTwoProviderModels);
    fix.alpha_guard.set("alpha-key");
    fix.beta_guard.set("beta-key");

    const auto session_path = fix.workspace.path() / "partial-session.jsonl";
    auto request = fix.make_request();
    request.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{session_path};
    // ADR 0064: the model/thinking/system entries buffer in memory and the
    // file appears only with the first user or assistant message, so a
    // session that never prompts leaves nothing behind to clean up. The
    // flush-failure contract
    // (partial file removed, triggering message rejected, batch retried) is
    // pinned at the store level in JsonlSessionStoreTest.

    auto created = fix.create(std::move(request));
    REQUIRE(created.has_value());
    // Neither publication nor close writes the file.
    created->session->close();
    CHECK_FALSE(std::filesystem::exists(session_path));
}

TEST_CASE("project settings defaults select the model only while the project is trusted",
        "[coding_agent][runtime][assembly-contract][issue509][spec]") {
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
    (void)fix.runtime.adopt_session(std::move(trusted->session));
    CHECK(trusted->resolved_identity.provider == "beta");
    CHECK(trusted->resolved_identity.model == "beta-2");

    // Untrusted: the project scope never loads, so the project default is
    // invisible to the resolution chain.
    auto untrusted = fix.create(fix.make_request(/*trust_override=*/false));
    REQUIRE(untrusted);
    (void)fix.runtime.adopt_session(std::move(untrusted->session));
    CHECK(untrusted->resolved_identity.provider != "beta");
    CHECK(untrusted->resolved_identity.model != "beta-2");
}

TEST_CASE("Assembly applies the gateway startup key without explicit model selection",
        "[coding_agent][runtime][assembly-contract][gateway][spec]") {
    AssemblyFixture fix;
    fix.write_models(kTwoProviderModels);
    auto wire = std::make_shared<tests::ScriptedTransport>();
    wire->attempts.push_back(tests::TransportAttempt{
            .chunks = {"data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}\n\n"},
    });
    ai::detail::ModelsTestAccess::set_transport(*fix.catalog.models, wire);
    auto request = fix.make_request();
    request.session_facts.api_key = "cli-startup-key";
    auto created = fix.create(std::move(request));
    REQUIRE(created);
    auto& session = fix.runtime.adopt_session(std::move(created->session));
    auto model = session.snapshot().agent_state.model;
    model.api = "openai-completions";
    auto stream = session.model_runtime()->stream_factory()(model, {}, {});
    auto result = tests::run_async_result(std::move(stream).run({}));
    REQUIRE(result);
    CHECK(result->stop_reason == ai::AssistantStopReason::Stop);
    REQUIRE(wire->requests.size() == 1);
    CHECK(wire->requests.front().headers.at("Authorization") == "Bearer cli-startup-key");
    CHECK_FALSE(std::filesystem::exists(fix.agent_dir / "auth.json"));
}

TEST_CASE("Assembly sends the CLI key on initial discovery before selecting its catalog model",
        "[coding_agent][runtime][assembly-contract][gateway][spec]") {
    namespace asio = boost::asio;
    namespace http = boost::beast::http;
    AssemblyFixture fix;
    asio::io_context server_io;
    asio::ip::tcp::acceptor acceptor(server_io, {asio::ip::make_address("127.0.0.1"), 0});
    asio::ip::tcp::socket socket(server_io);
    asio::steady_timer watchdog(server_io, std::chrono::seconds{8});
    // All borrowed server state outlives the coroutine and its joined thread.
    watchdog.async_wait([&acceptor, &socket](boost::system::error_code error) {
        if (error) return;
        boost::system::error_code ignored;
        acceptor.close(ignored);
        socket.close(ignored);
    });
    auto serve = [&acceptor, &socket, &watchdog]() -> asio::awaitable<std::pair<std::string, std::string>> {
        boost::system::error_code error;
        co_await acceptor.async_accept(socket, asio::redirect_error(asio::use_awaitable, error));
        if (error) co_return std::pair<std::string, std::string>{};
        boost::beast::flat_buffer buffer;
        http::request<http::string_body> incoming;
        co_await http::async_read(socket, buffer, incoming, asio::redirect_error(asio::use_awaitable, error));
        if (error) co_return std::pair<std::string, std::string>{};
        const std::string auth{incoming[http::field::authorization]};
        const std::string target{incoming.target()};
        http::response<http::string_body> response{http::status::ok, incoming.version()};
        response.keep_alive(false);
        response.body() =
                auth == "Bearer discovery-key" ? R"({"data":[{"id":"key-visible-model"}]})" : R"({"data":[]})";
        response.prepare_payload();
        co_await http::async_write(socket, response, asio::redirect_error(asio::use_awaitable, error));
        socket.close(error);
        watchdog.cancel();
        co_return std::pair{target, auth};
    };
    auto served = asio::co_spawn(server_io, serve(), asio::use_future);
    std::jthread server([&server_io] { server_io.run(); });
    auto request = fix.make_request();
    request.model_runtime.reset();
    request.magpie_base_url = "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port());
    request.session_facts.api_key = "discovery-key";
    auto created = fix.create(std::move(request));
    server.join();
    const auto [target, auth] = served.get();
    CHECK(target == "/v1/models");
    CHECK(auth == "Bearer discovery-key");
    REQUIRE(created);
    CHECK(created->resolved_identity.model == "key-visible-model");
    (void)fix.runtime.adopt_session(std::move(created->session));
}

TEST_CASE("Assembly preserves the identity of a discovered shared gateway runtime",
        "[coding_agent][runtime][assembly-contract][gateway][spec]") {
    AssemblyFixture fix;
    auto wire = std::make_shared<tests::ScriptedTransport>();
    wire->attempts = {
            {.chunks = {R"({"data":[{"id":"shared-model"}]})"}},
            {.chunks = {"data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}\n\n"}},
    };
    auto shared = coding_agent::create_model_runtime_for_testing(
            {.agent_dir = fix.agent_dir, .api_key = "bound-key"}, {}, wire);
    REQUIRE(shared);
    REQUIRE(wire->requests.size() == 1);
    CHECK(wire->requests.front().headers.at("Authorization") == "Bearer bound-key");
    for (const auto& requested : {std::optional<std::string>{},
                 std::optional<std::string>{"bound-key"},
                 std::optional<std::string>{"conflict-key"}}) {
        auto request = fix.make_request();
        request.model_runtime = *shared;
        request.session_facts.api_key = requested;
        auto created = fix.create(std::move(request));
        if (requested == "conflict-key") {
            REQUIRE_FALSE(created);
            CHECK(created.error().message.find("create another model runtime") != std::string::npos);
            CHECK(created.error().message.find("bound-key") == std::string::npos);
            CHECK(created.error().message.find("conflict-key") == std::string::npos);
        } else {
            REQUIRE(created);
            CHECK(created->resolved_identity.model == "shared-model");
            (void)fix.runtime.adopt_session(std::move(created->session));
        }
        CHECK((*shared)->model("magpie", "shared-model"));
        CHECK(wire->requests.size() == 1);
    }
    auto model = (*shared)->model("magpie", "shared-model");
    REQUIRE(model);
    auto result = tests::run_async_result((*shared)->stream_factory()(*model, {}, {}).run({}));
    REQUIRE(result);
    REQUIRE(wire->requests.size() == 2);
    CHECK(wire->requests.back().headers.at("Authorization") == "Bearer bound-key");
}

TEST_CASE("settings load errors surface as diagnostics ahead of resource diagnostics",
        "[coding_agent][runtime][assembly-contract][issue509][spec]") {
    AssemblyFixture fix;
    fix.write_models(kTwoProviderModels);
    fix.alpha_guard.set("alpha-key");
    fix.beta_guard.set("beta-key");

    // Broken global settings plus a duplicate project skill name: both kinds
    // of diagnostic fire in one creation attempt.
    {
        std::ofstream out(fix.agent_dir / "settings.json", std::ios::binary);
        out << "{ this is not json";
    }
    fix.workspace.write(
        ".pi/skills/first/SKILL.md",
        "---\nname: dupe-skill\ndescription: First skill.\n---\nFirst body.\n");
    fix.workspace.write(
        ".pi/skills/second/SKILL.md",
        "---\nname: dupe-skill\ndescription: Duplicate skill.\n---\nDuplicate body.\n");

    auto request = fix.make_request(/*trust_override=*/true);
    request.session_facts.no_skills = false;
    auto created = fix.create(std::move(request));
    REQUIRE(created);
    (void)fix.runtime.adopt_session(std::move(created->session));
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
