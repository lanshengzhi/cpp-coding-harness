#include "coding_agent/McpCredentialStore.hpp"

#include <cch/ai/CredentialStore.hpp>
#include <cch/mcp/UpstreamAuth.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace cch::coding_agent {
namespace {

using support::AsyncCompletion;
using support::AsyncResult;
using support::Error;
using support::ErrorCode;
using support::Expected;
using support::make_error;

/// The credential key one Server Id occupies in the shared store.
[[nodiscard]] std::string key_for(const std::string& server_id) { return mcp::credential_key(server_id); }

/// The bearer a stored record carries, or `std::nullopt` when the key holds no
/// record or holds a record of another type. A record this path did not write
/// is never guessed at.
[[nodiscard]] std::optional<std::string> bearer_of(const std::optional<ai::Credential>& record) {
    if (!record.has_value()) {
        return std::nullopt;
    }
    const auto* const api_key = std::get_if<ai::ApiKeyCredential>(&*record);
    if (api_key == nullptr || !api_key->key.has_value() || api_key->key->empty()) {
        return std::nullopt;
    }
    return *api_key->key;
}

[[nodiscard]] Error conflicting_record_error(const std::string& key) {
    return make_error(ErrorCode::Auth,
            "the Upstream MCP Server's stored credential is not a bearer credential",
            "the store already holds a record of another type under \"" + key +
                    "\"; refusing to replace it with a bearer token");
}

} // namespace

McpCredentialStore::McpCredentialStore(std::shared_ptr<AuthStorage> auth_storage)
    : auth_storage_(std::move(auth_storage)) {}

AsyncResult<std::optional<std::string>> McpCredentialStore::read_bearer(std::string server_id) {
    auto storage = auth_storage_;
    const std::string key = key_for(server_id);
    return AsyncResult<std::optional<std::string>>(AsyncResult<std::optional<std::string>>::producer_type(
            [storage, key](AsyncCompletion<std::optional<std::string>, Error> completion) mutable noexcept {
                storage->read(key).start(
                        [completion = std::move(completion), key](
                                std::expected<std::optional<ai::Credential>, Error> record) mutable noexcept {
                            if (!record) {
                                return completion(std::unexpected(record.error()));
                            }
                            completion(Expected<std::optional<std::string>>{bearer_of(*record)});
                        });
            }));
}

AsyncResult<void> McpCredentialStore::write_bearer(std::string server_id, std::string bearer) {
    auto storage = auth_storage_;
    const std::string key = key_for(server_id);
    return AsyncResult<void>(AsyncResult<void>::producer_type(
            [storage, key, bearer = std::move(bearer)](AsyncCompletion<void, Error> completion) mutable noexcept {
                // `modify` is the store's only write path and runs the hook
                // under its whole-file lock, so the replacement cannot race a
                // concurrent write of the same key.
                storage->modify(key,
                               ai::CredentialModifyHook([key, bearer = std::move(bearer)](
                                                                std::optional<ai::Credential> current) mutable
                                                                -> AsyncResult<std::optional<ai::Credential>> {
                                   if (current.has_value() && !std::holds_alternative<ai::ApiKeyCredential>(*current)) {
                                       return AsyncResult<std::optional<ai::Credential>>(
                                               Expected<std::optional<ai::Credential>>{
                                                       std::unexpected(conflicting_record_error(key))});
                                   }
                                   return AsyncResult<std::optional<ai::Credential>>(
                                           Expected<std::optional<ai::Credential>>{
                                                   ai::ApiKeyCredential{.key = std::move(bearer), .env = {}}});
                               }))
                        .start([completion = std::move(completion)](
                                       std::expected<std::optional<ai::Credential>, Error> written) mutable noexcept {
                            if (!written) {
                                return completion(std::unexpected(written.error()));
                            }
                            completion(Expected<void>{});
                        });
            }));
}

namespace {

/// The record type this store writes its OAuth credential under. It is the
/// same `"oauth"` spelling pi's own provider credentials use, so a Server Id's
/// record reads as an OAuth credential to any tool that inspects `auth.json`.
constexpr std::string_view kOAuthType{"oauth"};

[[nodiscard]] const std::string* string_member(const support::JsonValue& value, std::string_view name) {
    const auto* object = value.get_if<support::JsonValue::object_t>();
    if (object == nullptr) {
        return nullptr;
    }
    const auto found = object->find(std::string{name});
    if (found == object->end()) {
        return nullptr;
    }
    return found->second.get_if<std::string>();
}

/// A whole non-negative decimal, or zero. The build has no exceptions, so an
/// expiry that is not a plain number reads as no expiry: the credential is
/// used until the authorization server refuses it.
[[nodiscard]] std::int64_t parse_expiry(const std::string* text) {
    if (text == nullptr || text->empty()) {
        return 0;
    }
    std::int64_t value = 0;
    for (const auto byte : *text) {
        if (byte < '0' || byte > '9') {
            return 0;
        }
        if (value > (std::numeric_limits<std::int64_t>::max() - (byte - '0')) / 10) {
            return 0;
        }
        value = value * 10 + (byte - '0');
    }
    return value;
}

/// The stored OAuth record, but only when it is one and only when it declares
/// `issuer`. Every other shape — another record type, a record with no
/// `iss`, an `iss` that is not this issuer — reads as absent rather than as a
/// token the connection would present to the wrong authorization server.
[[nodiscard]] std::optional<mcp::UpstreamOAuthCredential> oauth_credential_of(
        const std::optional<support::JsonValue>& record, std::string_view issuer) {
    if (!record.has_value()) {
        return std::nullopt;
    }
    const auto* type = string_member(*record, "type");
    const auto* stored_issuer = string_member(*record, "iss");
    const auto* access = string_member(*record, "access");
    if (type == nullptr || *type != kOAuthType || stored_issuer == nullptr || *stored_issuer != issuer ||
            access == nullptr || access->empty()) {
        return std::nullopt;
    }
    mcp::UpstreamOAuthCredential credential{
            .issuer = *stored_issuer,
            .access_token = *access,
    };
    if (const auto* client_id = string_member(*record, "client_id"); client_id != nullptr) {
        credential.client_id = *client_id;
    }
    if (const auto* refresh = string_member(*record, "refresh"); refresh != nullptr) {
        credential.refresh_token = *refresh;
    }
    credential.expires_at = parse_expiry(string_member(*record, "expires"));
    if (const auto* scope = string_member(*record, "scope"); scope != nullptr && !scope->empty()) {
        std::size_t start = 0;
        while (start <= scope->size()) {
            const auto separator = scope->find(' ', start);
            const auto end = separator == std::string::npos ? scope->size() : separator;
            if (end > start) {
                credential.scopes.push_back(scope->substr(start, end - start));
            }
            if (separator == std::string::npos) {
                break;
            }
            start = separator + 1;
        }
    }
    return credential;
}

[[nodiscard]] support::JsonValue oauth_record_of(const mcp::UpstreamOAuthCredential& credential) {
    std::string scope;
    for (const auto& one : credential.scopes) {
        if (one.empty()) {
            continue;
        }
        if (!scope.empty()) {
            scope.push_back(' ');
        }
        scope += one;
    }
    support::JsonValue::object_t record{
            {"type", support::JsonValue{std::string{kOAuthType}}},
            {"iss", support::JsonValue{credential.issuer}},
            {"client_id", support::JsonValue{credential.client_id}},
            {"access", support::JsonValue{credential.access_token}},
            {"refresh", support::JsonValue{credential.refresh_token}},
            // The expiry is a string, not a number: `auth.json` is read by
            // pi, and a whole number is what pi's own serializer writes for a
            // provider credential's expiry.
            {"expires", support::JsonValue{std::to_string(credential.expires_at)}},
    };
    if (!scope.empty()) {
        record.emplace("scope", support::JsonValue{std::move(scope)});
    }
    return support::JsonValue{std::move(record)};
}

} // namespace

AsyncResult<std::optional<mcp::UpstreamOAuthCredential>> McpCredentialStore::read_oauth(
        std::string server_id, std::string issuer) {
    auto storage = auth_storage_;
    const std::string key = key_for(server_id);
    return AsyncResult<std::optional<mcp::UpstreamOAuthCredential>>(
            AsyncResult<std::optional<mcp::UpstreamOAuthCredential>>::producer_type(
                    [storage, key, issuer = std::move(issuer)](
                            AsyncCompletion<std::optional<mcp::UpstreamOAuthCredential>, Error>
                                    completion) mutable noexcept {
                        storage->read_record(key).start([completion = std::move(completion), issuer](
                                                                std::expected<std::optional<support::JsonValue>, Error>
                                                                        record) mutable noexcept {
                            if (!record) {
                                return completion(std::unexpected(record.error()));
                            }
                            completion(Expected<std::optional<mcp::UpstreamOAuthCredential>>{
                                    oauth_credential_of(*record, issuer)});
                        });
                    }));
}

AsyncResult<void> McpCredentialStore::write_oauth(
        std::string server_id, const mcp::UpstreamOAuthCredential& credential) {
    auto storage = auth_storage_;
    const std::string key = key_for(server_id);
    return AsyncResult<void>(
            AsyncResult<void>::producer_type([storage, key, record = oauth_record_of(credential)](
                                                     AsyncCompletion<void, Error> completion) mutable noexcept {
                storage->write_record(key, std::move(record))
                        .start([completion = std::move(completion)](
                                       std::expected<void, Error> written) mutable noexcept {
                            if (!written) {
                                return completion(std::unexpected(written.error()));
                            }
                            completion(Expected<void>{});
                        });
            }));
}

} // namespace cch::coding_agent
