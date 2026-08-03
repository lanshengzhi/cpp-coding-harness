#pragma once

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
/// entry unchanged; failures propagate without writing the backing store.
using CredentialModifyHook = std::move_only_function<
    boost::asio::awaitable<util::Expected<std::optional<Credential>>>(
        std::optional<Credential>)>;

/// App-owned credential persistence keyed by Provider id. Trusted callers may
/// receive secrets from `read`/`modify`; `list` is deliberately metadata-only.
class CredentialStore {
public:
    virtual ~CredentialStore() = default;

    /// Read the stored credential, possibly expired. Missing or unsupported
    /// records resolve to `std::nullopt`.
    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<std::optional<Credential>>> read(
        std::string provider_id) = 0;

    /// Enumerate provider id and type only. Implementations must not resolve or
    /// execute configured key values while listing.
    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<std::vector<CredentialInfo>>> list() = 0;

    /// The only write path. The callback runs while the backing store's
    /// provider mutation is serialized and sees the latest persisted value.
    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<std::optional<Credential>>> modify(
        std::string provider_id,
        CredentialModifyHook modifier) = 0;

    /// Remove one provider credential, serialized against `modify`.
    [[nodiscard]] virtual boost::asio::awaitable<util::ExpectedVoid> remove(
        std::string provider_id) = 0;
};

} // namespace cch::ai
