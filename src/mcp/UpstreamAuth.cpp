#include <cch/mcp/UpstreamAuth.hpp>

#include "mcp/Protocol.hpp"
#include "mcp/Redaction.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cch::mcp {
namespace {

using support::AsyncCompletion;
using support::AsyncResult;
using support::Error;
using support::ErrorCode;
using support::Expected;
using support::make_error;

/// The prefix that opens the `mcp.<server-id>` credential namespace
/// (spec #833 story 33). It lives here, beside the key builder, so the
/// namespace has one definition for the credential path, its tests, and the
/// OAuth path that follows it (#849).
constexpr std::string_view kCredentialNamespace{"mcp."};

/// The scheme prefix of the header a resolved bearer is written to.
constexpr std::string_view kBearerScheme{"Bearer "};

[[nodiscard]] bool same_header_name(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto lhs = static_cast<unsigned char>(left[index]);
        const auto rhs = static_cast<unsigned char>(right[index]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }
    return true;
}

/// The value of a `bearer-env:<VAR>` reference. The variable's *value* is the
/// secret: it is returned to the resolver and to nothing else, and an unset or
/// empty variable resolves as no value at all.
[[nodiscard]] std::string read_environment(const std::string& name) {
    const char* const value = std::getenv(name.c_str());
    return value == nullptr ? std::string{} : std::string{value};
}

/// This package's credential failure. It names the Server Id and the
/// environment variable and never the token, and every word it carries is
/// redacted by the known credential value before it is bounded, so no path out
/// of the credential path can leak the secret (issue #838).
[[nodiscard]] Error credential_error(ErrorCode code,
        std::string message,
        std::string_view server_id,
        std::string_view env_var,
        std::string_view secret = {}) {
    std::string detail = "Server Id \"" + std::string{server_id} + "\"";
    if (!env_var.empty()) {
        detail += " declares bearer-env:" + std::string{env_var};
    }
    return make_error(code, std::move(message), redaction::redacted_text(std::move(detail), secret));
}

/// A credential-store read failure wrapped in this package's own wording while
/// keeping the store's reason, which names a file and never a credential value.
[[nodiscard]] Error store_read_failure(
        const Error& cause, std::string_view server_id, std::string_view env_var, std::string_view secret) {
    auto failure = credential_error(
            cause.code, "the Upstream MCP Server's credential store could not be read", server_id, env_var, secret);
    const std::string reason = cause.detail.empty() ? cause.message : cause.message + ": " + cause.detail;
    failure.detail += "; " + redaction::redacted_text(reason, secret);
    return failure;
}

/// The bearer one request is authenticated with, decided from the declared
/// environment value and what the credential store already holds.
///
/// An absent answer is the third case: the environment value is authoritative
/// and the store does not hold it yet, so it has to be persisted before the
/// request is written.
[[nodiscard]] std::optional<Expected<UpstreamAuth>> decide_bearer(std::string from_environment,
        const std::optional<std::string>& stored,
        std::string_view server_id,
        std::string_view env_var) {
    if (from_environment.empty()) {
        if (stored.has_value() && !stored->empty()) {
            return Expected<UpstreamAuth>{UpstreamAuth{.bearer = *stored}};
        }
        return Expected<UpstreamAuth>{std::unexpected(credential_error(ErrorCode::Auth,
                "the Upstream MCP Server has no bearer credential: the environment variable is unset and no "
                "credential is stored",
                server_id,
                env_var))};
    }
    if (stored.has_value() && *stored == from_environment) {
        return Expected<UpstreamAuth>{UpstreamAuth{.bearer = std::move(from_environment)}};
    }
    return std::nullopt;
}

} // namespace

std::string credential_key(std::string_view server_id) {
    return std::string{kCredentialNamespace} + std::string{server_id};
}

AsyncResult<UpstreamAuth> resolve_upstream_auth(std::string server_id,
        std::optional<std::string> bearer_env_var,
        std::shared_ptr<UpstreamCredentialStore> store,
        std::stop_token stop_token) {
    if (stop_token.stop_requested()) {
        return AsyncResult<UpstreamAuth>(std::unexpected(
                make_error(ErrorCode::Cancelled, "the Upstream MCP Server credential resolution was cancelled", "")));
    }
    if (!bearer_env_var.has_value()) {
        // The server declares no credential, so nothing is resolved and no
        // `Authorization` header is written.
        return AsyncResult<UpstreamAuth>(Expected<UpstreamAuth>{UpstreamAuth{}});
    }
    if (store == nullptr) {
        return AsyncResult<UpstreamAuth>(std::unexpected(credential_error(ErrorCode::Auth,
                "the Upstream MCP Server declares a bearer credential but the MCP Host has no credential store",
                server_id,
                *bearer_env_var)));
    }

    std::string env_var = *bearer_env_var;
    const std::string from_environment = read_environment(env_var);
    // The store is read on every request, so an environment-resolved token the
    // store already holds is not rewritten and a request is never
    // authenticated from a store the host could not read.
    return AsyncResult<UpstreamAuth>(AsyncResult<UpstreamAuth>::producer_type(
            [server_id = std::move(server_id),
                    env_var = std::move(env_var),
                    from_environment = std::move(from_environment),
                    store = std::move(store)](AsyncCompletion<UpstreamAuth, Error> completion) mutable noexcept {
                store->read_bearer(server_id).start([completion = std::move(completion),
                                                            server_id,
                                                            env_var,
                                                            from_environment,
                                                            store](std::expected<std::optional<std::string>, Error>
                                                                    stored) mutable noexcept {
                    if (!stored) {
                        return completion(std::unexpected(
                                store_read_failure(stored.error(), server_id, env_var, from_environment)));
                    }
                    auto decided = decide_bearer(from_environment, *stored, server_id, env_var);
                    if (decided.has_value()) {
                        return completion(std::move(*decided));
                    }
                    store->write_bearer(server_id, from_environment)
                            .start([completion = std::move(completion), server_id, env_var, from_environment](
                                           std::expected<void, Error> written) mutable noexcept {
                                if (!written) {
                                    return completion(std::unexpected(credential_error(written.error().code,
                                            "the Upstream MCP Server's bearer credential could not be persisted",
                                            server_id,
                                            env_var,
                                            from_environment)));
                                }
                                completion(Expected<UpstreamAuth>{UpstreamAuth{.bearer = from_environment}});
                            });
                });
            }));
}

void apply_upstream_auth(McpRequest& request, const UpstreamAuth& auth) {
    if (!auth.bearer.has_value() || auth.bearer->empty()) {
        return;
    }
    // An `Authorization` header already on the request wins: a header set
    // before the bearer is resolved is never overwritten, and HTTP field names
    // are case-insensitive, so an occupied slot is recognized whatever its
    // spelling.
    const auto occupied = std::find_if(request.headers.begin(), request.headers.end(), [](const auto& header) {
        return same_header_name(header.first, protocol::kHeaderAuthorization);
    });
    if (occupied != request.headers.end()) {
        return;
    }
    request.headers.emplace(std::string{protocol::kHeaderAuthorization}, std::string{kBearerScheme} + *auth.bearer);
}

AsyncResult<UpstreamAuth> resolve_oauth_bearer(std::string server_id,
        std::string issuer,
        std::shared_ptr<UpstreamCredentialStore> store,
        std::stop_token stop_token) {
    if (stop_token.stop_requested()) {
        return AsyncResult<UpstreamAuth>(std::unexpected(
                make_error(ErrorCode::Cancelled, "the Upstream MCP Server credential resolution was cancelled", "")));
    }
    if (store == nullptr) {
        return AsyncResult<UpstreamAuth>(std::unexpected(credential_error(ErrorCode::Auth,
                "the Upstream MCP Server authorizes with OAuth but the MCP Host has no credential store",
                server_id,
                {})));
    }
    return AsyncResult<UpstreamAuth>(AsyncResult<UpstreamAuth>::producer_type(
            [server_id = std::move(server_id), issuer = std::move(issuer), store = std::move(store)](
                    AsyncCompletion<UpstreamAuth, Error> completion) mutable noexcept {
                store->read_oauth(server_id, issuer)
                        .start([completion = std::move(completion), server_id, issuer, store](
                                       std::expected<std::optional<UpstreamOAuthCredential>, Error>
                                               stored) mutable noexcept {
                            if (!stored) {
                                return completion(
                                        std::unexpected(store_read_failure(stored.error(), server_id, {}, {})));
                            }
                            if (!stored->has_value() || (*stored)->access_token.empty()) {
                                // No credential for *this* issuer is not an error and
                                // is not a token: the connection has nothing to
                                // authenticate with, which the caller reports as the
                                // authentication challenge it already saw.
                                return completion(Expected<UpstreamAuth>{UpstreamAuth{}});
                            }
                            completion(Expected<UpstreamAuth>{UpstreamAuth{.bearer = (*stored)->access_token}});
                        });
            }));
}

} // namespace cch::mcp
