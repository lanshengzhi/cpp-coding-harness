#pragma once

#include <cch/coding_agent/AuthStorage.hpp>
#include <cch/mcp/UpstreamAuth.hpp>
#include <cch/support/AsyncResult.hpp>

#include <memory>
#include <optional>
#include <string>

namespace cch::coding_agent {

/// The MCP Host's credential store, backed by pi's shared `auth.json`
/// (issue #838, spec #833 story 33, ADR 0030).
///
/// The `cch_mcp` package owns upstream credential *handling* — the
/// `mcp.<server-id>` namespace, the environment reference, the resolution
/// order, and the request header; the Agent Config Directory path derivation,
/// the whole-file lock, the owner-only permissions, and the lossless
/// serializer stay here, which is why this is the only place the two packages
/// meet (ADR 0065). One Server Id is one `ai::ApiKeyCredential` under the
/// flat `mcp.<server-id>` key, so the store's key map stays a plain
/// provider-id map and an MCP credential can never collide with a provider's.
///
/// The implementation holds its `AuthStorage` by shared ownership: the store
/// must outlive every in-flight read or write the MCP client starts on a
/// connection's execution domain, and `AuthStorage` is move-only.
class McpCredentialStore final : public mcp::UpstreamCredentialStore {
public:
    explicit McpCredentialStore(std::shared_ptr<AuthStorage> auth_storage);

    /// The stored bearer under `mcp.<server-id>`, `std::nullopt` when the key
    /// holds no API-key credential. A record of another type is not a bearer
    /// credential and resolves as absent rather than as a guess.
    [[nodiscard]] cch::support::AsyncResult<std::optional<std::string>> read_bearer(std::string server_id) override;

    /// Persist one Server Id's bearer through `CredentialStore::modify`, the
    /// store's only write path. A record of another type is left alone: a
    /// record this path did not write is not one it may overwrite, which keeps
    /// an OAuth credential (#849) from being replaced by a bearer.
    [[nodiscard]] cch::support::AsyncResult<void> write_bearer(std::string server_id, std::string bearer) override;

    /// The stored OAuth record under `mcp.<server-id>`, but only when it
    /// declares exactly `issuer`. A record of another type, a record with no
    /// issuer, and a record issued by a different authorization server all read
    /// as absent: issuers never cross-use one another's credentials (issue
    /// #849).
    [[nodiscard]] cch::support::AsyncResult<std::optional<mcp::UpstreamOAuthCredential>> read_oauth(
            std::string server_id, std::string issuer) override;

    /// Persist one OAuth record through the store's record write path, which
    /// is the same whole-file lock and the same owner-only permissions every
    /// credential write here uses. The record is written whole, so a bearer
    /// under the same key would be replaced: a Server Id has one credential,
    /// and the record decides which kind.
    [[nodiscard]] cch::support::AsyncResult<void> write_oauth(
            std::string server_id, const mcp::UpstreamOAuthCredential& credential) override;

private:
    std::shared_ptr<AuthStorage> auth_storage_;
};

} // namespace cch::coding_agent
