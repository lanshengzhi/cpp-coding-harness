#include "coding_agent/McpCredentialStore.hpp"

#include <cch/ai/CredentialStore.hpp>
#include <cch/mcp/UpstreamAuth.hpp>
#include <cch/support/Error.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <string>
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

} // namespace cch::coding_agent
