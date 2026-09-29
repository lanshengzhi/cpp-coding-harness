#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/coding_agent/McpServerTrust.hpp>
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cch/support/AsyncResult.hpp>

#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

using coding_agent::McpServerTrustAnswer;
using coding_agent::McpServerTrustDecision;
using coding_agent::McpServerTrustGate;
using coding_agent::McpServerTrustPrompter;
using coding_agent::McpServerTrustPromptRequest;
using coding_agent::McpServerTrustResolution;
using coding_agent::McpServerTrustSource;
using coding_agent::McpServerTrustStore;
using coding_agent::McpServerTrustUpdate;
using coding_agent::UserMcpServerSettings;

using TrustOutcome = std::expected<McpServerTrustResolution, support::Error>;

constexpr std::string_view kDefaultUrl = "https://mcp.example/mcp";

[[nodiscard]] UserMcpServerSettings server(std::string server_id, std::string url = std::string{kDefaultUrl}) {
    return UserMcpServerSettings{.server_id = std::move(server_id), .url = std::move(url)};
}

[[nodiscard]] McpServerTrustUpdate trust(std::string server_id, McpServerTrustDecision decision) {
    return McpServerTrustUpdate{.server_id = std::move(server_id), .decision = decision};
}

/// A prompter the test answers by hand: it records what it was asked and holds
/// the completion until the test fires it, so a pending prompt stays pending.
struct ScriptedPrompter final {
    struct State {
        std::vector<McpServerTrustPromptRequest> requests;
        std::optional<support::AsyncCompletion<McpServerTrustAnswer, support::Error>> completion;
        std::size_t prompts{0};
    };

    std::shared_ptr<State> state{std::make_shared<State>()};

    [[nodiscard]] McpServerTrustPrompter prompter() const {
        auto state = this->state;
        return [state](McpServerTrustPromptRequest request, std::stop_token) {
            return support::AsyncResult<McpServerTrustAnswer>{
                    support::AsyncProducer<McpServerTrustAnswer, support::Error>{
                            [state, request = std::move(request)](support::AsyncCompletion<McpServerTrustAnswer,
                                    support::Error> completion) mutable noexcept {
                                state->requests.push_back(std::move(request));
                                ++state->prompts;
                                state->completion.emplace(std::move(completion));
                            }}};
        };
    }

    // Both helpers answer the prompt the gate started; a test with no prompt
    // pending has already failed the checks that follow.
    void answer(McpServerTrustAnswer answer) {
        auto completion = std::move(*state->completion);
        state->completion.reset();
        completion(std::expected<McpServerTrustAnswer, support::Error>{answer});
    }

    void fail(support::Error error) {
        auto completion = std::move(*state->completion);
        state->completion.reset();
        completion(std::unexpected(std::move(error)));
    }
};

[[nodiscard]] McpServerTrustPrompter answering(McpServerTrustAnswer answer) {
    return [answer](McpServerTrustPromptRequest, std::stop_token) {
        return support::AsyncResult<McpServerTrustAnswer>{std::expected<McpServerTrustAnswer, support::Error>{answer}};
    };
}

/// Consume an operation through its completion callback — the only way to
/// observe an answer the gate has not received yet. `outcome` is the test's
/// own slot, so it stays valid while a prompt is left unanswered.
void consume(support::AsyncResult<McpServerTrustResolution> operation, std::optional<TrustOutcome>& outcome) {
    operation.start([&outcome](TrustOutcome result) mutable noexcept { outcome.emplace(std::move(result)); });
}

} // namespace

TEST_CASE("McpServerTrustStore round-trips one decision per Server Id", "[coding_agent][mcp-trust][issue840][spec]") {
    tests::TempWorkspace workspace;
    McpServerTrustStore store{workspace.path() / "mcp-trust.json"};

    CHECK_FALSE(store.get_entry("executor").value().has_value());

    auto written = store.set_many({trust("executor", McpServerTrustDecision::Trusted)});
    REQUIRE(written.has_value());

    auto entry = store.get_entry("executor");
    REQUIRE(entry.has_value());
    REQUIRE(entry->has_value());
    CHECK((*entry)->server_id == "executor");
    CHECK((*entry)->decision == McpServerTrustDecision::Trusted);
    CHECK_FALSE(store.get_entry("other").value().has_value());

    auto forgotten = store.set_many({trust("executor", McpServerTrustDecision::Unknown)});
    REQUIRE(forgotten.has_value());
    CHECK_FALSE(store.get_entry("executor").value().has_value());
}

TEST_CASE("McpServerTrustStore rejects a Server Id it would otherwise have to trust as a key",
        "[coding_agent][mcp-trust][issue840][spec]") {
    tests::TempWorkspace workspace;
    McpServerTrustStore store{workspace.path() / "mcp-trust.json"};

    auto written = store.set_many({trust("../escape", McpServerTrustDecision::Trusted)});
    CHECK_FALSE(written.has_value());
    CHECK_FALSE(std::filesystem::exists(store.path()));
}

TEST_CASE("McpServerTrustStore fails closed on a malformed store", "[coding_agent][mcp-trust][issue840][spec]") {
    tests::TempWorkspace workspace;
    auto trust_path = workspace.path() / "mcp-trust.json";
    std::ofstream(trust_path) << "{not json";
    McpServerTrustStore store{trust_path};

    auto entry = store.get_entry("executor");
    REQUIRE_FALSE(entry.has_value());
    // The diagnostic names the store it came from and carries its own detail.
    CHECK(entry.error().message.find("MCP server trust store") != std::string::npos);
    CHECK_FALSE(entry.error().detail.empty());

    McpServerTrustGate gate{store};
    gate.assess({server("executor")});
    auto resolution = gate.resolution("executor");
    REQUIRE(resolution.has_value());
    CHECK(resolution->decision == McpServerTrustDecision::Untrusted);
    CHECK(resolution->source == McpServerTrustSource::StoreUnavailable);
    CHECK_FALSE(resolution->diagnostics.empty());
    CHECK(gate.enabled_server_ids().empty());
}

TEST_CASE("McpServerTrustGate honours a stored decision without asking again",
        "[coding_agent][mcp-trust][issue840][spec]") {
    tests::TempWorkspace workspace;
    auto trust_path = workspace.path() / "mcp-trust.json";
    McpServerTrustStore store{trust_path};
    auto written = store.set_many({trust("executor", McpServerTrustDecision::Trusted)});
    REQUIRE(written.has_value());

    ScriptedPrompter scripted;
    McpServerTrustGate gate{McpServerTrustStore{trust_path}, scripted.prompter()};
    gate.assess({server("executor")});

    CHECK(gate.enabled_server_ids() == std::vector<std::string>{"executor"});
    CHECK(gate.pending_requests().empty());
    CHECK(scripted.state->prompts == 0);

    auto resolution = gate.resolution("executor");
    REQUIRE(resolution.has_value());
    CHECK(resolution->source == McpServerTrustSource::StoredDecision);

    std::optional<TrustOutcome> outcome;
    consume(gate.ask("executor"), outcome);
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->has_value());
    CHECK((*outcome)->source == McpServerTrustSource::StoredDecision);
    CHECK(scripted.state->prompts == 0);
}

TEST_CASE("McpServerTrustGate acceptance persists the decision for the next session",
        "[coding_agent][mcp-trust][issue840][spec]") {
    tests::TempWorkspace workspace;
    auto trust_path = workspace.path() / "mcp-trust.json";
    const auto configured = std::vector<UserMcpServerSettings>{server("executor")};

    // First session: the user accepts the first-enable prompt.
    McpServerTrustGate first_session{McpServerTrustStore{trust_path}, answering(McpServerTrustAnswer::Accepted)};
    first_session.assess(configured);
    CHECK(first_session.enabled_server_ids().empty());
    const auto pending = first_session.pending_requests();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].server_id == "executor");
    CHECK(pending[0].url == kDefaultUrl);

    std::optional<TrustOutcome> outcome;
    consume(first_session.ask("executor"), outcome);
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->has_value());
    CHECK((*outcome)->decision == McpServerTrustDecision::Trusted);
    CHECK((*outcome)->source == McpServerTrustSource::PromptAccepted);
    CHECK((*outcome)->enables_server());
    CHECK(first_session.enabled_server_ids() == std::vector<std::string>{"executor"});

    // Second session: the decision is honored after a restart, and the prompt
    // the user would answer differently is never shown again.
    ScriptedPrompter second_prompt;
    McpServerTrustGate second_session{McpServerTrustStore{trust_path}, second_prompt.prompter()};
    second_session.assess(configured);
    CHECK(second_session.enabled_server_ids() == std::vector<std::string>{"executor"});
    CHECK(second_prompt.state->prompts == 0);
    auto stored = McpServerTrustStore{trust_path}.get_entry("executor");
    REQUIRE(stored.has_value());
    REQUIRE(stored->has_value());
    CHECK((*stored)->decision == McpServerTrustDecision::Trusted);
}

TEST_CASE("McpServerTrustGate decline persists a disabled server", "[coding_agent][mcp-trust][issue840][spec]") {
    tests::TempWorkspace workspace;
    auto trust_path = workspace.path() / "mcp-trust.json";
    const auto configured = std::vector<UserMcpServerSettings>{server("executor")};

    McpServerTrustGate first_session{McpServerTrustStore{trust_path}, answering(McpServerTrustAnswer::Declined)};
    first_session.assess(configured);

    std::optional<TrustOutcome> outcome;
    consume(first_session.ask("executor"), outcome);
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->has_value());
    CHECK((*outcome)->decision == McpServerTrustDecision::Untrusted);
    CHECK((*outcome)->source == McpServerTrustSource::PromptDeclined);
    CHECK_FALSE((*outcome)->enables_server());
    CHECK(first_session.enabled_server_ids().empty());

    // The decline is remembered, so the next session does not ask again and
    // still does not enable the server.
    ScriptedPrompter second_prompt;
    McpServerTrustGate second_session{McpServerTrustStore{trust_path}, second_prompt.prompter()};
    second_session.assess(configured);
    CHECK(second_session.enabled_server_ids().empty());
    CHECK(second_prompt.state->prompts == 0);
    auto stored = McpServerTrustStore{trust_path}.get_entry("executor");
    REQUIRE(stored.has_value());
    REQUIRE(stored->has_value());
    CHECK((*stored)->decision == McpServerTrustDecision::Untrusted);
}

TEST_CASE("McpServerTrustGate cancel leaves the server disabled and undecided",
        "[coding_agent][mcp-trust][issue840][spec]") {
    tests::TempWorkspace workspace;
    auto trust_path = workspace.path() / "mcp-trust.json";
    const auto configured = std::vector<UserMcpServerSettings>{server("executor")};

    McpServerTrustGate first_session{McpServerTrustStore{trust_path}, answering(McpServerTrustAnswer::Cancelled)};
    first_session.assess(configured);

    std::optional<TrustOutcome> outcome;
    consume(first_session.ask("executor"), outcome);
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->has_value());
    CHECK((*outcome)->decision == McpServerTrustDecision::Untrusted);
    CHECK((*outcome)->source == McpServerTrustSource::PromptCancelled);
    CHECK_FALSE((*outcome)->enables_server());
    CHECK(first_session.enabled_server_ids().empty());
    // A dismissal is not a decision, so nothing is persisted and the next
    // session asks again.
    CHECK_FALSE(std::filesystem::exists(trust_path));

    ScriptedPrompter second_prompt;
    McpServerTrustGate second_session{McpServerTrustStore{trust_path}, second_prompt.prompter()};
    second_session.assess(configured);
    CHECK(second_session.enabled_server_ids().empty());
    CHECK(second_session.pending_requests().size() == 1);
}

TEST_CASE("McpServerTrustGate treats a non-interactive session without consent as declined",
        "[coding_agent][mcp-trust][issue840][spec]") {
    tests::TempWorkspace workspace;
    auto trust_path = workspace.path() / "mcp-trust.json";
    const auto configured = std::vector<UserMcpServerSettings>{server("executor")};

    // No prompter is installed: a headless or print-mode session.
    McpServerTrustGate gate{McpServerTrustStore{trust_path}};
    gate.assess(configured);

    std::optional<TrustOutcome> outcome;
    consume(gate.ask("executor"), outcome);
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->has_value());
    CHECK((*outcome)->decision == McpServerTrustDecision::Untrusted);
    CHECK((*outcome)->source == McpServerTrustSource::PromptUnavailable);
    CHECK(gate.enabled_server_ids().empty());
    // Nothing is persisted: an interactive session may still obtain consent.
    CHECK_FALSE(std::filesystem::exists(trust_path));
}

TEST_CASE("McpServerTrustGate keeps a never-answered prompt out of session startup",
        "[coding_agent][mcp-trust][issue840][spec]") {
    tests::TempWorkspace workspace;
    auto trust_path = workspace.path() / "mcp-trust.json";
    const auto configured = std::vector<UserMcpServerSettings>{
            server("quick", "https://quick.example/mcp"), server("slow", "https://slow.example/mcp")};
    McpServerTrustStore store{trust_path};
    auto written = store.set_many({trust("quick", McpServerTrustDecision::Trusted)});
    REQUIRE(written.has_value());

    ScriptedPrompter scripted;
    McpServerTrustGate gate{McpServerTrustStore{trust_path}, scripted.prompter()};

    // Assessment is a store read: it neither prompts nor waits, so the trusted
    // server is enabled while the undecided one only becomes pending.
    gate.assess(configured);
    CHECK(gate.enabled_server_ids() == std::vector<std::string>{"quick"});
    const auto pending = gate.pending_requests();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].server_id == "slow");
    CHECK(pending[0].url == "https://slow.example/mcp");
    CHECK(scripted.state->prompts == 0);

    // Asking returns a pending operation; the enable surface does not change
    // until the user answers.
    std::optional<TrustOutcome> outcome;
    consume(gate.ask("slow"), outcome);
    CHECK_FALSE(outcome.has_value());
    CHECK(gate.enabled_server_ids() == std::vector<std::string>{"quick"});
    REQUIRE(scripted.state->requests.size() == 1);
    CHECK(scripted.state->requests[0].server_id == "slow");
    CHECK(scripted.state->requests[0].url == "https://slow.example/mcp");

    scripted.answer(McpServerTrustAnswer::Accepted);
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->has_value());
    CHECK((*outcome)->source == McpServerTrustSource::PromptAccepted);
    CHECK(gate.enabled_server_ids() == std::vector<std::string>{"quick", "slow"});
}

TEST_CASE("McpServerTrustGate leaves a server disabled when its prompt fails",
        "[coding_agent][mcp-trust][issue840][spec]") {
    tests::TempWorkspace workspace;
    auto trust_path = workspace.path() / "mcp-trust.json";
    const auto configured = std::vector<UserMcpServerSettings>{server("executor")};

    ScriptedPrompter scripted;
    McpServerTrustGate gate{McpServerTrustStore{trust_path}, scripted.prompter()};
    gate.assess(configured);

    std::optional<TrustOutcome> outcome;
    consume(gate.ask("executor"), outcome);
    scripted.fail(support::make_error(support::ErrorCode::Cancelled, "prompt was dismissed by the user"));

    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->has_value());
    CHECK(gate.enabled_server_ids().empty());
    auto resolution = gate.resolution("executor");
    REQUIRE(resolution.has_value());
    CHECK(resolution->decision == McpServerTrustDecision::Untrusted);
    CHECK(resolution->source == McpServerTrustSource::PromptFailed);
    CHECK_FALSE(resolution->diagnostics.empty());
    CHECK_FALSE(std::filesystem::exists(trust_path));
}

TEST_CASE("McpServerTrustGate declines a server whose decision cannot be persisted",
        "[coding_agent][mcp-trust][issue840][spec]") {
    tests::TempWorkspace workspace;
    auto trust_path = workspace.path() / "mcp-trust.json";
    const auto configured = std::vector<UserMcpServerSettings>{server("executor")};

    McpServerTrustGate gate{McpServerTrustStore{trust_path}, answering(McpServerTrustAnswer::Accepted)};
    gate.assess(configured);
    REQUIRE(gate.pending_requests().size() == 1);

    // The store becomes unwritable between the assessment and the answer.
    auto elsewhere = workspace.path() / "elsewhere.json";
    std::ofstream(elsewhere) << "{}";
    std::filesystem::create_symlink(elsewhere, trust_path);

    // A decision that cannot be persisted is not a consent the user can rely
    // on, so the server stays disabled and the failure is reported.
    std::optional<TrustOutcome> outcome;
    consume(gate.ask("executor"), outcome);
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->has_value());
    CHECK(gate.enabled_server_ids().empty());
    auto resolution = gate.resolution("executor");
    REQUIRE(resolution.has_value());
    CHECK(resolution->decision == McpServerTrustDecision::Untrusted);
    CHECK(resolution->source == McpServerTrustSource::PromptFailed);
}

TEST_CASE("McpServerTrustGate refuses to ask about a server that is not configured",
        "[coding_agent][mcp-trust][issue840][spec]") {
    tests::TempWorkspace workspace;
    ScriptedPrompter scripted;
    McpServerTrustGate gate{McpServerTrustStore{workspace.path() / "mcp-trust.json"}, scripted.prompter()};
    gate.assess({server("executor")});

    CHECK_FALSE(gate.resolution("other").has_value());

    std::optional<TrustOutcome> outcome;
    consume(gate.ask("other"), outcome);
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->has_value());
    CHECK(scripted.state->prompts == 0);
}

TEST_CASE("the MCP server trust store is derived from the agent config directory path seam",
        "[coding_agent][mcp-trust][issue840][spec]") {
    const auto path = coding_agent::mcp_server_trust_file_path();
    if (path.empty()) {
        // No home directory is resolvable; every derived path is then empty.
        return;
    }
    CHECK(path.parent_path() == coding_agent::agent_config_dir());
    CHECK(path.filename() == "mcp-trust.json");
    CHECK(path != coding_agent::trust_store_file_path());
}
