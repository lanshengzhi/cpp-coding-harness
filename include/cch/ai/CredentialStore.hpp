#pragma once

#include <cch/support/AsyncResult.hpp>
#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cch::ai {

/// Stored API-key credential. Provider-scoped environment values accompany the
/// key but never enter Model values, session metadata, diagnostics, or logs.
struct ApiKeyCredential {
    std::optional<std::string> key{std::nullopt};
    std::map<std::string, std::string> env{};

    bool operator==(const ApiKeyCredential&) const = default;
};

/// Stored canonical OAuth credential. `account_id` is Codex's JSON
/// `accountId`; other provider-specific JSON fields are preserved privately by
/// the file-backed serializer.
struct OAuthCredential {
    std::string refresh{};
    std::string access{};
    std::int64_t expires{0};
    std::optional<std::string> account_id{std::nullopt};

    bool operator==(const OAuthCredential&) const = default;
};

// §3.3 names variant aliases `*Variant`; `Credential` predates the rule and
// keeps its short name to avoid public API churn (debt recorded in #372).
using Credential = std::variant<ApiKeyCredential, OAuthCredential>;

/// Non-secret metadata for one stored provider record. `type` remains a string
/// so metadata-only enumeration can report records created by a newer pi
/// without exposing or interpreting their secret fields.
struct CredentialInfo {
    std::string provider_id{};
    std::string type{};

    bool operator==(const CredentialInfo&) const = default;
};

/// A serialized credential update. Returning `std::nullopt` leaves the current
/// entry unchanged; failures propagate without writing the backing store. The
/// hook owns its inputs and its returned awaitable; the backing store drives
/// it on its own execution context while the whole-file mutation lock is held.
using CredentialModifyHook = std::move_only_function<
    boost::asio::awaitable<util::Expected<std::optional<Credential>>>(
        std::optional<Credential>)>;

/// App-owned credential persistence keyed by Provider id. Trusted callers may
/// receive secrets from `read`/`modify`; `list` is deliberately metadata-only.
///
/// Read and modification operations complete through typed `AsyncResult`
/// outcomes with owned inputs (ADR 0040 / #454). Ready reads serve an
/// in-memory snapshot inline; a write's blocking filesystem work stays off
/// the caller's execution by completing through the operation's own
/// `AsyncResult` while the OAuth refresh remains an asynchronous hook.
class CredentialStore {
public:
    virtual ~CredentialStore() = default;

    /// Read the stored credential, possibly expired. Missing or unsupported
    /// records resolve to `std::nullopt`.
    [[nodiscard]] virtual cch::support::AsyncResult<std::optional<Credential>> read(
        std::string provider_id) = 0;

    /// Enumerate provider id and type only. Implementations must not resolve or
    /// execute configured key values while listing.
    [[nodiscard]] virtual cch::support::AsyncResult<std::vector<CredentialInfo>> list() = 0;

    /// The only write path. The callback runs while the backing store's
    /// provider mutation is serialized and sees the latest persisted value.
    [[nodiscard]] virtual cch::support::AsyncResult<std::optional<Credential>> modify(
        std::string provider_id,
        CredentialModifyHook modifier) = 0;

    /// Remove one provider credential, serialized against `modify`.
    [[nodiscard]] virtual cch::support::AsyncResult<void> remove(
        std::string provider_id) = 0;
};

} // namespace cch::ai
