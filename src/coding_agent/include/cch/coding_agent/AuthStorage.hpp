#pragma once

#include <cch/ai/CredentialStore.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/JsonValue.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace cch::coding_agent {

/// File-backed CredentialStore for pi's shared `auth.json` contract.
///
/// The implementation owns whole-file proper-lockfile-compatible locking,
/// owner-only permissions, a last-valid in-memory snapshot, and the private
/// lossless serializer. Agent Config Directory path derivation remains in the
/// coding-agent layer; callers pass the concrete `<agentDir>/auth.json` path.
///
/// Reads serve the last-valid snapshot as ready `AsyncResult` outcomes;
/// writes complete through `AsyncResult` with owned inputs while the whole-file
/// mutation lock is held (ADR 0040 / #454).
///
/// The typed `ai::CredentialStore` surface above remains the only write path
/// for a *provider credential*: nothing bypasses `modify` to write one. The
/// record surface below is a separate, deliberately narrow escape hatch for a
/// record the typed credential cannot express — the MCP OAuth credential of
/// issue #849, which carries an issuer and a dynamically registered client id
/// and is stored under pi's shared flat `auth.json` key map. It is a
/// `cch_mcp`-owned value in a `cch_coding_agent`-owned file, which is the only
/// place the two packages may meet (ADR 0065).
class AuthStorage final : public ai::CredentialStore {
public:
    explicit AuthStorage(std::filesystem::path auth_path);
    AuthStorage(AuthStorage&&) = delete;
    AuthStorage& operator=(AuthStorage&&) = delete;
    ~AuthStorage() override;

    AuthStorage(const AuthStorage&) = delete;
    AuthStorage& operator=(const AuthStorage&) = delete;

    /// Re-read the file under its whole-file lock. Invalid or unreadable
    /// content preserves the previous valid snapshot. This construction-time
    /// helper is intentionally synchronous and best-effort.
    void reload() noexcept;

    [[nodiscard]] cch::support::AsyncResult<std::optional<ai::Credential>> read(
        std::string provider_id) override;
    [[nodiscard]] cch::support::AsyncResult<std::vector<ai::CredentialInfo>> list() override;
    [[nodiscard]] cch::support::AsyncResult<std::optional<ai::Credential>> modify(
        std::string provider_id,
        ai::CredentialModifyHook modifier) override;
    [[nodiscard]] cch::support::AsyncResult<void> remove(
        std::string provider_id) override;

    /// One record as the file holds it, unparsed, or `std::nullopt` when the
    /// key holds none. Served from the last-valid snapshot, so a read never
    /// waits on the file lock. The value is the record verbatim, unknown
    /// members included; the caller owns its interpretation.
    [[nodiscard]] cch::support::AsyncResult<std::optional<cch::support::JsonValue>> read_record(
            std::string provider_id);

    /// Replace one record with `record` verbatim, under the same whole-file
    /// lock and with the same owner-only permissions every other write here
    /// uses. A record of another shape under the key is the caller's to
    /// preserve: this method writes what it is given and does not merge, so a
    /// caller that must not destroy a foreign record reads it first.
    [[nodiscard]] cch::support::AsyncResult<void> write_record(std::string provider_id, cch::support::JsonValue record);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::coding_agent
