#include <cch/coding_agent/McpServerTrust.hpp>

#include "coding_agent/TrustStoreFile.hpp"

#include <cch/support/AsyncResult.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <string>
#include <utility>

namespace cch::coding_agent {
namespace {

/// The store's name in its own file diagnostics.
constexpr std::string_view kStoreName = "MCP server trust store";

[[nodiscard]] McpServerTrustDecision decision_of(bool trusted) noexcept {
    return trusted ? McpServerTrustDecision::Trusted : McpServerTrustDecision::Untrusted;
}

[[nodiscard]] support::Error unknown_server_error(std::string_view server_id) {
    return trust_store_error("no configured Upstream MCP Server with this Server Id",
            std::format("Server Id \"{}\" is not configured in mcpServers", server_id));
}

[[nodiscard]] support::Error invalid_server_id_error(std::string_view server_id) {
    return trust_store_error("invalid Upstream MCP Server Id for a trust decision",
            std::format(
                    "a Server Id must be 1 to 48 characters of [A-Za-z0-9_-]; got {} characters", server_id.size()));
}

[[nodiscard]] McpServerTrustResolution store_unavailable_resolution(
        const std::string& server_id, const McpServerTrustStore& store, support::Error error) {
    return McpServerTrustResolution{
            .server_id = server_id,
            .decision = McpServerTrustDecision::Untrusted,
            .source = McpServerTrustSource::StoreUnavailable,
            .diagnostics = {ProjectTrustDiagnostic{
                    .severity = ProjectTrustDiagnosticSeverity::Warning,
                    .code = "mcp_server_trust_store_unavailable",
                    .message = std::move(error).message,
                    .path = store.path().string(),
            }},
    };
}

/// Whether the user still owes this server its first-enable consent. A server
/// whose prompt could not be shown or answered has no decision either, so it
/// may be asked again; a declined, cancelled, or accepted server does not.
[[nodiscard]] bool awaits_consent(const McpServerTrustResolution& resolution) noexcept {
    return resolution.decision == McpServerTrustDecision::Unknown ||
           resolution.source == McpServerTrustSource::PromptUnavailable ||
           resolution.source == McpServerTrustSource::PromptFailed;
}

} // namespace

McpServerTrustStore::McpServerTrustStore(std::filesystem::path trust_path) : trust_path_(std::move(trust_path)) {}

support::Expected<std::optional<McpServerTrustStoreEntry>> McpServerTrustStore::get_entry(
        std::string_view server_id) const {
    auto data = read_trust_store_map(trust_path_, kStoreName);
    if (!data) {
        return std::unexpected(data.error());
    }
    const auto found = data->find(std::string(server_id));
    if (found == data->end() || !found->second.has_value()) {
        return std::nullopt;
    }
    return McpServerTrustStoreEntry{
            .server_id = std::string(server_id),
            .decision = decision_of(*found->second),
    };
}

support::ExpectedVoid McpServerTrustStore::set_many(const std::vector<McpServerTrustUpdate>& updates) const {
    auto data = read_trust_store_map(trust_path_, kStoreName);
    if (!data) {
        return std::unexpected(data.error());
    }
    for (const auto& update : updates) {
        if (!is_valid_mcp_server_id(update.server_id)) {
            return std::unexpected(invalid_server_id_error(update.server_id));
        }
        if (update.decision == McpServerTrustDecision::Unknown) {
            data->erase(update.server_id);
        } else {
            (*data)[update.server_id] = update.decision == McpServerTrustDecision::Trusted;
        }
    }
    return write_trust_store_map(trust_path_, *data, kStoreName);
}

McpServerTrustGate::McpServerTrustGate(McpServerTrustStore store, McpServerTrustPrompter prompter)
    : store_(std::move(store)), prompter_(std::move(prompter)) {}

void McpServerTrustGate::assess(const std::vector<UserMcpServerSettings>& servers) {
    configured_.clear();
    resolutions_.clear();
    for (const auto& server : servers) {
        configured_.push_back(McpServerTrustPromptRequest{
                .server_id = server.server_id,
                .url = server.url,
        });

        auto entry = store_.get_entry(server.server_id);
        if (!entry) {
            resolutions_.emplace(
                    server.server_id, store_unavailable_resolution(server.server_id, store_, std::move(entry.error())));
            continue;
        }
        resolutions_.emplace(server.server_id,
                (*entry).has_value()
                        ? McpServerTrustResolution{
                                  .server_id = server.server_id,
                                  .decision = (*entry)->decision,
                                  .source = McpServerTrustSource::StoredDecision,
                          }
                        : McpServerTrustResolution{
                                  .server_id = server.server_id,
                                  .decision = McpServerTrustDecision::Unknown,
                                  .source = McpServerTrustSource::PromptPending,
                          });
    }
}

std::vector<std::string> McpServerTrustGate::enabled_server_ids() const {
    std::vector<std::string> enabled;
    for (const auto& request : configured_) {
        const auto found = resolutions_.find(request.server_id);
        if (found != resolutions_.end() && found->second.enables_server()) {
            enabled.push_back(request.server_id);
        }
    }
    return enabled;
}

std::vector<McpServerTrustPromptRequest> McpServerTrustGate::pending_requests() const {
    std::vector<McpServerTrustPromptRequest> pending;
    for (const auto& request : configured_) {
        const auto found = resolutions_.find(request.server_id);
        if (found != resolutions_.end() && found->second.decision == McpServerTrustDecision::Unknown) {
            pending.push_back(request);
        }
    }
    return pending;
}

std::optional<McpServerTrustResolution> McpServerTrustGate::resolution(std::string_view server_id) const {
    const auto found = resolutions_.find(std::string(server_id));
    if (found == resolutions_.end()) {
        return std::nullopt;
    }
    return found->second;
}

support::AsyncResult<McpServerTrustResolution> McpServerTrustGate::ask(
        std::string_view server_id, std::stop_token stop_token) {
    const auto key = std::string(server_id);
    const auto found = resolutions_.find(key);
    if (found == resolutions_.end()) {
        return support::AsyncResult<McpServerTrustResolution>{std::unexpected(unknown_server_error(server_id))};
    }
    if (!awaits_consent(found->second)) {
        return support::AsyncResult<McpServerTrustResolution>{
                std::expected<McpServerTrustResolution, support::Error>{found->second}};
    }
    if (!prompter_) {
        // A non-interactive session cannot obtain consent, so the server is
        // declined for the session and nothing is persisted.
        resolutions_[key] = McpServerTrustResolution{
                .server_id = found->second.server_id,
                .decision = McpServerTrustDecision::Untrusted,
                .source = McpServerTrustSource::PromptUnavailable,
        };
        return support::AsyncResult<McpServerTrustResolution>{
                std::expected<McpServerTrustResolution, support::Error>{resolutions_[key]}};
    }

    return support::AsyncResult<McpServerTrustResolution>{support::AsyncProducer<McpServerTrustResolution,
            support::Error>{
            [this, server_id = key, request = request_for(key), stop_token](
                    support::AsyncCompletion<McpServerTrustResolution, support::Error> completion) mutable noexcept {
                prompter_(request, stop_token)
                        .start([this, server_id = std::move(server_id), completion = std::move(completion)](
                                       std::expected<McpServerTrustAnswer, support::Error> answer) mutable noexcept {
                            if (!answer) {
                                record_failed_prompt(server_id, answer.error());
                                completion(std::unexpected(answer.error()));
                                return;
                            }
                            auto recorded = record_answer(server_id, *answer);
                            if (!recorded) {
                                completion(std::unexpected(recorded.error()));
                                return;
                            }
                            completion(std::expected<McpServerTrustResolution, support::Error>{std::move(*recorded)});
                        });
            }}};
}

McpServerTrustPromptRequest McpServerTrustGate::request_for(std::string_view server_id) const {
    const auto found = std::ranges::find_if(configured_,
            [server_id](const McpServerTrustPromptRequest& request) { return request.server_id == server_id; });
    if (found == configured_.end()) {
        return {};
    }
    return *found;
}

support::Expected<McpServerTrustResolution> McpServerTrustGate::record_answer(
        const std::string& server_id, McpServerTrustAnswer answer) {
    // Only an answer that is a decision is persisted; a cancelled prompt
    // leaves the store untouched so the next session asks again.
    const auto decision = answer == McpServerTrustAnswer::Accepted ? McpServerTrustDecision::Trusted
                                                                   : McpServerTrustDecision::Untrusted;
    if (answer != McpServerTrustAnswer::Cancelled) {
        if (auto written = store_.set_many({McpServerTrustUpdate{
                    .server_id = server_id,
                    .decision = decision,
            }});
                !written) {
            resolutions_[server_id] = McpServerTrustResolution{
                    .server_id = server_id,
                    .decision = McpServerTrustDecision::Untrusted,
                    .source = McpServerTrustSource::PromptFailed,
                    .diagnostics = {ProjectTrustDiagnostic{
                            .severity = ProjectTrustDiagnosticSeverity::Warning,
                            .code = "mcp_server_trust_decision_not_persisted",
                            .message = written.error().message,
                            .path = store_.path().string(),
                    }},
            };
            return std::unexpected(written.error());
        }
    }

    resolutions_[server_id] = McpServerTrustResolution{
            .server_id = server_id,
            .decision = decision,
            .source = answer == McpServerTrustAnswer::Accepted   ? McpServerTrustSource::PromptAccepted
                      : answer == McpServerTrustAnswer::Declined ? McpServerTrustSource::PromptDeclined
                                                                 : McpServerTrustSource::PromptCancelled,
    };
    return resolutions_[server_id];
}

void McpServerTrustGate::record_failed_prompt(const std::string& server_id, const support::Error& error) {
    resolutions_[server_id] = McpServerTrustResolution{
            .server_id = server_id,
            .decision = McpServerTrustDecision::Untrusted,
            .source = McpServerTrustSource::PromptFailed,
            .diagnostics = {ProjectTrustDiagnostic{
                    .severity = ProjectTrustDiagnosticSeverity::Warning,
                    .code = "mcp_server_trust_prompt_failed",
                    .message = error.message,
                    .path = store_.path().string(),
            }},
    };
}

} // namespace cch::coding_agent
