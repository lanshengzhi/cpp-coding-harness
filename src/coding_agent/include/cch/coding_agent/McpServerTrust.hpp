#pragma once

#include <cch/coding_agent/ProjectTrust.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent {

/// The user's consent decision for one configured Upstream MCP Server (issue
/// #840, spec #833 story 4). `Unknown` is the "not decided yet" state: the
/// server stays disabled and its first-enable consent is still outstanding.
enum class McpServerTrustDecision {
    Trusted,
    Untrusted,
    Unknown,
};

/// Where one server's trust decision came from. Every source other than a
/// resolved consent leaves the server disabled, which is what makes "no
/// upstream request" the fail-closed result rather than a separate policy.
enum class McpServerTrustSource {
    /// The store already held this Server Id's decision; it is honored without
    /// asking again.
    StoredDecision,
    /// No decision is recorded yet: the server's first-enable consent is
    /// outstanding, and the server is not enabled until it is answered.
    PromptPending,
    /// The user accepted the first-enable prompt in this session, and the
    /// decision was persisted.
    PromptAccepted,
    /// The user declined the first-enable prompt, and the decision was
    /// persisted.
    PromptDeclined,
    /// The user dismissed the prompt. A dismissal is not a decision, so
    /// nothing is persisted and the next session asks again.
    PromptCancelled,
    /// The session has no way to ask (no prompter installed), so an undecided
    /// server is declined for the session without a persisted decision.
    PromptUnavailable,
    /// The prompt failed. The server stays disabled and no decision is
    /// persisted.
    PromptFailed,
    /// The trust store could not be read. The server is declined rather than
    /// enabled on unverified state.
    StoreUnavailable,
};

/// One recorded trust decision in the store, keyed by Server Id.
struct McpServerTrustStoreEntry {
    std::string server_id{};
    McpServerTrustDecision decision{McpServerTrustDecision::Unknown};
};

/// One trust decision to persist. `Unknown` forgets the Server Id, so the next
/// session asks again — the same persisted/session-only split pi's project
/// trust options make.
struct McpServerTrustUpdate {
    std::string server_id{};
    McpServerTrustDecision decision{McpServerTrustDecision::Unknown};
};

/// Persisted first-enable trust decisions for configured Upstream MCP Servers,
/// keyed by Server Id (issue #840). It follows the project-trust store's
/// contract exactly — one owner-only JSON object mapping a key to a boolean,
/// a missing key meaning "no decision yet" — in its own file under the Agent
/// Config Directory, so the pi-shaped project-trust map keeps its flat
/// path-to-boolean meaning and a Server Id can never collide with a path.
/// Entries for a Server Id the configuration no longer declares are inert:
/// only `McpServerTrustGate` reads them, and only for configured servers.
class McpServerTrustStore {
public:
    explicit McpServerTrustStore(std::filesystem::path trust_path);

    [[nodiscard]] const std::filesystem::path& path() const { return trust_path_; }

    /// The recorded decision for one Server Id, `std::nullopt` when it has
    /// none. A store that cannot be read is an error, never a silent "no
    /// decision".
    [[nodiscard]] support::Expected<std::optional<McpServerTrustStoreEntry>> get_entry(
            std::string_view server_id) const;

    /// Persist a batch of decisions under the store's re-read-and-merge lock.
    /// A Server Id that fails the Server Id charset is rejected rather than
    /// written, so the store never holds arbitrary text as a key.
    [[nodiscard]] support::ExpectedVoid set_many(const std::vector<McpServerTrustUpdate>& updates) const;

private:
    std::filesystem::path trust_path_;
};

/// One configured Upstream MCP Server awaiting the user's first-enable
/// consent, and the facts the prompt needs to be answerable.
struct McpServerTrustPromptRequest {
    std::string server_id{};
    std::string url{};
};

/// The user's answer to one first-enable prompt. `Declined` and `Cancelled`
/// both leave the server disabled; only `Declined` persists, because
/// dismissing a prompt is not a decision.
enum class McpServerTrustAnswer {
    Accepted,
    Declined,
    Cancelled,
};

/// The frontend's first-enable trust prompt. It receives the pending Server Id
/// and its configured endpoint, and answers for that server alone. A session
/// with no such prompt installs an empty prompter, which the gate treats as a
/// decline rather than as consent.
using McpServerTrustPrompter = std::move_only_function<support::AsyncResult<McpServerTrustAnswer>(
        McpServerTrustPromptRequest, std::stop_token)>;

/// One server's trust outcome, with the project's trust diagnostics — the
/// vocabulary `resolve_project_trust` already reports through.
struct McpServerTrustResolution {
    std::string server_id{};
    McpServerTrustDecision decision{McpServerTrustDecision::Unknown};
    McpServerTrustSource source{McpServerTrustSource::PromptPending};
    std::vector<ProjectTrustDiagnostic> diagnostics{};

    /// Whether this server may be enabled. The single predicate every
    /// downstream consumer reads: only recorded consent enables a server, so
    /// a pending, declined, cancelled, unanswered, or store-unavailable server
    /// is attributable to zero upstream requests.
    [[nodiscard]] bool enables_server() const noexcept { return decision == McpServerTrustDecision::Trusted; }
};

/// The first-enable trust gate for configured Upstream MCP Servers (issue
/// #840, spec #833 story 4, ProjectTrust precedent).
///
/// The gate resolves every configured server from the store without blocking
/// and without prompting, so session startup never waits for consent: an
/// undecided server is reported as pending and is not enabled. Asking for a
/// server's consent is a separate, caller-driven step whose answer the gate
/// records and persists.
class McpServerTrustGate {
public:
    /// `prompter` is the frontend's first-enable prompt. Constructed with an
    /// empty prompter the gate describes a non-interactive session, where an
    /// undecided server is declined instead of prompted.
    explicit McpServerTrustGate(McpServerTrustStore store, McpServerTrustPrompter prompter = {});

    /// Resolve every configured server against the store, replacing any
    /// previous assessment. Reads the store only: it never prompts, never
    /// performs an upstream request, and never waits. A store it cannot read
    /// declines every server with a diagnostic instead of enabling on
    /// unverified state.
    void assess(const std::vector<UserMcpServerSettings>& servers);

    /// The servers whose decision enables them right now, in configuration
    /// order. This is the whole enable surface: a server absent from it is
    /// disabled and produces no upstream request.
    [[nodiscard]] std::vector<std::string> enabled_server_ids() const;

    /// The servers still awaiting the user's first-enable consent, in
    /// configuration order.
    [[nodiscard]] std::vector<McpServerTrustPromptRequest> pending_requests() const;

    /// The current resolution for one Server Id, `std::nullopt` when it is not
    /// a configured server.
    [[nodiscard]] std::optional<McpServerTrustResolution> resolution(std::string_view server_id) const;

    /// Ask one server's first-enable prompt and record the answer. Accept
    /// persists a trusted decision and decline persists a declined one, so
    /// both are honored after a restart; cancel persists nothing, matching the
    /// project-trust prompt's session-only option. A server that already has a
    /// persisted or answered decision is returned as is, without asking. With
    /// no prompter installed the answer is a decline for the session. A
    /// prompt or a store write that fails leaves the server disabled, records
    /// the failure on its resolution, and reports the error; such a server can
    /// be asked again. The gate must outlive the returned operation, which
    /// records the answer into it.
    [[nodiscard]] support::AsyncResult<McpServerTrustResolution> ask(
            std::string_view server_id, std::stop_token stop_token = {});

private:
    /// The prompt request for one assessed Server Id, empty when it is not
    /// configured.
    [[nodiscard]] McpServerTrustPromptRequest request_for(std::string_view server_id) const;
    /// Apply one answer: persist it when it is a decision, and update the
    /// assessed resolution to the resulting outcome.
    [[nodiscard]] support::Expected<McpServerTrustResolution> record_answer(
            const std::string& server_id, McpServerTrustAnswer answer);
    /// Record a prompt that could not be answered; the server stays disabled
    /// and nothing is persisted.
    void record_failed_prompt(const std::string& server_id, const support::Error& error);
    McpServerTrustStore store_;
    McpServerTrustPrompter prompter_;
    /// Assessed servers in configuration order, and their current resolutions.
    std::vector<McpServerTrustPromptRequest> configured_;
    std::map<std::string, McpServerTrustResolution> resolutions_;
};

} // namespace cch::coding_agent
