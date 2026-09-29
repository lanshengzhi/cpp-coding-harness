#include <cch/mcp/UpstreamAuth.hpp>
#include <cch/mcp/UpstreamOAuth.hpp>

#include "mcp/Diagnostics.hpp"
#include "mcp/OAuthCallbackServer.hpp"
#include "mcp/OAuthSupport.hpp"
#include "mcp/Protocol.hpp"
#include "mcp/Redaction.hpp"
#include "mcp/WwwAuthenticate.hpp"
#include "support/Json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::mcp {
namespace {

using support::AsyncCompletion;
using support::AsyncResult;
using support::Error;
using support::ErrorCode;
using support::Expected;
using support::JsonValue;
using support::make_error;

/// The dynamic-registration `application_type` of a loopback client (the
/// 2026-07-28 auth hardening: the declaration is required, and `native` is the
/// one that describes a client that cannot keep a secret).
constexpr std::string_view kApplicationType{"native"};

/// The client authentication method a public loopback client registers with.
constexpr std::string_view kTokenEndpointAuthMethod{"none"};

/// The loopback route the authorization server redirects to. The port is the
/// one the listener actually bound.
constexpr std::string_view kCallbackRoute{"/callback"};

/// The well-known paths an authorization server publishes its metadata at
/// (RFC 8414 §3.1). The OpenID Connect spelling is the fallback, because an
/// authorization server that publishes only that one is still usable.
constexpr std::string_view kAuthorizationServerMetadataPath{"/.well-known/oauth-authorization-server"};
constexpr std::string_view kOpenIdConfigurationPath{"/.well-known/openid-configuration"};

/// The well-known path a protected resource publishes its metadata at
/// (RFC 9728 §3).
constexpr std::string_view kProtectedResourceMetadataPath{"/.well-known/oauth-protected-resource"};

/// The exchange bound. An authorization endpoint is an ordinary HTTPS
/// endpoint; the bound is containment, so a hung authorization server cannot
/// hold an authorization open indefinitely.
constexpr std::chrono::milliseconds kExchangeTimeout{std::chrono::seconds{30}};

[[nodiscard]] Error oauth_error(std::string message, std::string detail, ErrorCode code = ErrorCode::OAuth) {
    return make_error(code, std::move(message), std::move(detail));
}

/// Every error this flow reports is redacted and bounded by the package's one
/// diagnostics point, so a token an authorization server echoed into an error
/// body cannot reach a status surface or a session record (issue #838,
/// CODING_STANDARDS.md §10.7).
[[nodiscard]] Error oauth_failure(
        std::string message, std::string detail, std::string_view secret, ErrorCode code = ErrorCode::OAuth) {
    return make_error(
            code, std::move(message), diagnostics::bounded(redaction::redacted_text(std::move(detail), secret)));
}

[[nodiscard]] Error cancelled_error() { return make_error(ErrorCode::Cancelled, "the authorization was cancelled"); }

[[nodiscard]] std::string trim_trailing_slashes(std::string_view url) {
    std::string trimmed{url};
    while (!trimmed.empty() && trimmed.back() == '/') {
        trimmed.pop_back();
    }
    return trimmed;
}

[[nodiscard]] std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
}

[[nodiscard]] std::string join_scopes(const std::vector<std::string>& scopes) {
    std::string joined;
    for (const auto& scope : scopes) {
        if (scope.empty()) {
            continue;
        }
        if (!joined.empty()) {
            joined.push_back(' ');
        }
        joined += scope;
    }
    return joined;
}

/// The scopes one authorization requests: what the protected resource asked
/// for, and otherwise what the authorization server supports. A scope neither
/// side declared is never requested.
[[nodiscard]] std::vector<std::string> requested_scopes(
        const UpstreamOAuthMetadata& metadata, std::string_view challenge_scope) {
    auto scopes = oauth::split_scopes(challenge_scope);
    if (scopes.empty()) {
        return metadata.scopes;
    }
    return scopes;
}

[[nodiscard]] std::string_view challenge_scope_of(const UpstreamOAuthRequest& request) {
    return request.challenge.has_value() ? std::string_view{request.challenge->scope} : std::string_view{};
}

/// One authorization in flight. Every step takes the flow by shared
/// ownership, so a cancellation that settles an operation while a later step
/// is still starting cannot destroy state either step is reading.
struct Flow {
    UpstreamOAuthRequest request{};
    std::shared_ptr<McpTransport> transport{};
    std::shared_ptr<UpstreamCredentialStore> store{};
    std::shared_ptr<UpstreamOAuthPrompter> prompter{};
    std::shared_ptr<oauth::LoopbackCallbackServer> listener{};
    /// Weak on purpose: the cancellation watcher must not keep the flow alive,
    /// or a flow whose caller has gone would never be released.
    std::optional<std::stop_callback<std::function<void()>>> stop_watcher{};
    UpstreamOAuthMetadata metadata{};
    std::string client_id{};
    std::string verifier{};
    std::string code_challenge{};
    std::string state{};
    std::string redirect_uri{};
    AsyncCompletion<UpstreamOAuthGrant, Error> completion{};
};

using FlowPtr = std::shared_ptr<Flow>;

/// Runs the first action offered and absorbs the rest, which is what the race
/// between the browser's redirect and the user's dismissal needs: whichever
/// arrives first ends the authorization, and the loser must not complete a
/// settled operation.
class Once {
public:
    void run(std::function<void()> action) {
        {
            std::scoped_lock lock(mutex_);
            if (settled_) {
                return;
            }
            settled_ = true;
        }
        action();
    }

private:
    std::mutex mutex_;
    bool settled_{false};
};

/// Settle the flow exactly once, close the listener, and tell the prompter how
/// it ended whenever it was given something to show. A flow that failed before
/// the prompt was presented still reports, so a frontend that opened nothing is
/// not left waiting on a report it never gets.
void settle(const FlowPtr& flow, Expected<UpstreamOAuthGrant> outcome) {
    auto completion = std::move(flow->completion);
    if (!completion) {
        return;
    }
    flow->completion = nullptr;
    if (flow->listener) {
        flow->listener->close();
        flow->listener.reset();
    }
    if (flow->prompter) {
        // A cancellation is its own outcome on the status surface: the user
        // closed the dialog, which is not a failure of anything.
        const auto outcome_kind =
                outcome ? outcome->outcome
                        : (outcome.error().code == ErrorCode::Cancelled ? UpstreamOAuthOutcome::Cancelled
                                                                        : UpstreamOAuthOutcome::Failed);
        flow->prompter->finish(UpstreamOAuthReport{
                .server_id = flow->request.server_id,
                .issuer = flow->metadata.issuer,
                .outcome = outcome_kind,
                .message = outcome ? (outcome->outcome == UpstreamOAuthOutcome::Authorized
                                                     ? "the Upstream MCP Server is authorized"
                                                     : "the authorization was not completed")
                                   : diagnostics::bounded(outcome.error().message),
        });
    }
    completion(std::move(outcome));
}

[[nodiscard]] AsyncResult<McpResponse> exchange(std::shared_ptr<McpTransport> transport,
        std::string method,
        std::string url,
        std::string body,
        std::string content_type) {
    return AsyncResult<McpResponse>(AsyncResult<McpResponse>::producer_type(
            [transport = std::move(transport),
                    method = std::move(method),
                    url = std::move(url),
                    body = std::move(body),
                    content_type = std::move(content_type)](
                    AsyncCompletion<McpResponse, Error> completion) mutable noexcept {
                McpRequest request;
                request.url = std::move(url);
                request.method = std::move(method);
                request.body = std::move(body);
                request.timeout = kExchangeTimeout;
                request.headers.emplace(
                        std::string{protocol::kHeaderProtocolVersion}, std::string{protocol::kProtocolVersion});
                request.headers.emplace("Accept", "application/json");
                if (!content_type.empty()) {
                    request.headers.emplace("Content-Type", std::move(content_type));
                }
                transport->send(std::move(request))
                        .start([completion = std::move(completion)](
                                       std::expected<McpResponse, Error> response) mutable noexcept {
                            if (!response) {
                                return completion(
                                        std::unexpected(oauth_failure("the authorization server could not be reached",
                                                response.error().message + " " + response.error().detail,
                                                {},
                                                response.error().code == ErrorCode::Cancelled ? ErrorCode::Cancelled
                                                                                              : ErrorCode::Network)));
                            }
                            completion(Expected<McpResponse>{std::move(*response)});
                        });
            }));
}

/// Read one JSON document out of a 2xx response. A non-2xx status and a body
/// that is not the JSON the exchange promised are both failures that name the
/// endpoint and never echo the body: an OAuth error body can carry a
/// description that quotes a token.
[[nodiscard]] Expected<JsonValue> read_json_response(const McpResponse& response, std::string_view what) {
    if (response.status_code < 200 || response.status_code > 299) {
        return std::unexpected(oauth_error("the authorization server refused the request",
                std::string{what} + " answered with HTTP status " + std::to_string(response.status_code)));
    }
    auto parsed = support::read_json(response.body);
    if (!parsed) {
        return std::unexpected(oauth_error("the authorization server answered with a body that is not JSON",
                std::string{what} + " did not answer with a JSON document"));
    }
    return parsed;
}

// ── discovery ─────────────────────────────────────────────────────────────────

/// One document the discovery chain may try, and the issuer it must declare.
struct DiscoveryStep {
    std::string url{};
    std::string issuer{};
};

void register_client(const FlowPtr& flow);
void register_new_client(const FlowPtr& flow);
void present_and_wait(const FlowPtr& flow);
void on_callback(const FlowPtr& flow, oauth::CallbackRequest callback);
void continue_discovery(const FlowPtr& flow,
        std::vector<DiscoveryStep> candidates,
        std::size_t index,
        std::optional<Error> last_failure);

/// The authorization server's metadata document, reduced to the endpoint
/// triple this flow uses. A document without an issuer, an authorization
/// endpoint, and a token endpoint is refused rather than half-used.
[[nodiscard]] Expected<UpstreamOAuthMetadata> metadata_from_document(
        const JsonValue& document, const std::string& requested_issuer) {
    auto issuer = oauth::string_member(document, "issuer");
    if (!issuer.has_value() || issuer->empty()) {
        return std::unexpected(oauth_error("the authorization server metadata declares no issuer",
                "the metadata document has no non-empty \"issuer\" member"));
    }
    if (!requested_issuer.empty() && *issuer != requested_issuer) {
        // The document is fetched from the issuer's own well-known path, so an
        // issuer that is not the one that was asked for is a redirection this
        // flow does not follow.
        return std::unexpected(oauth_error("the authorization server metadata declares a different issuer",
                "the document fetched for \"" + requested_issuer + "\" declares the issuer \"" + *issuer + "\""));
    }
    auto authorization_endpoint = oauth::string_member(document, "authorization_endpoint");
    auto token_endpoint = oauth::string_member(document, "token_endpoint");
    if (!authorization_endpoint.has_value() || authorization_endpoint->empty() || !token_endpoint.has_value() ||
            token_endpoint->empty()) {
        return std::unexpected(oauth_error("the authorization server metadata is incomplete",
                "the document declares no \"authorization_endpoint\" or no \"token_endpoint\""));
    }
    UpstreamOAuthMetadata metadata{
            .issuer = *issuer,
            .authorization_endpoint = *authorization_endpoint,
            .token_endpoint = *token_endpoint,
    };
    if (const auto registration = oauth::string_member(document, "registration_endpoint");
            registration.has_value() && !registration->empty()) {
        metadata.registration_endpoint = *registration;
    }
    metadata.scopes = oauth::string_array_member(document, "scopes_supported");
    return Expected<UpstreamOAuthMetadata>{std::move(metadata)};
}

void continue_discovery(const FlowPtr& flow,
        std::vector<DiscoveryStep> candidates,
        std::size_t index,
        std::optional<Error> last_failure) {
    if (index >= candidates.size()) {
        settle(flow,
                std::unexpected(last_failure.has_value()
                                        ? *last_failure
                                        : oauth_error("the authorization server's metadata could not be read",
                                                  "no well-known authorization-server metadata document answered "
                                                  "for the issuer")));
        return;
    }
    const auto step = candidates[index];
    exchange(flow->transport, "GET", step.url, {}, {})
            .start([flow, candidates = std::move(candidates), index, step, last_failure](
                           std::expected<McpResponse, Error> response) mutable noexcept {
                std::optional<Error> failure;
                if (!response) {
                    failure = response.error();
                } else if (auto document = read_json_response(*response, "the authorization server metadata");
                        !document) {
                    failure = document.error();
                } else if (auto metadata = metadata_from_document(*document, step.issuer); !metadata) {
                    failure = metadata.error();
                } else {
                    flow->metadata = std::move(*metadata);
                    register_client(flow);
                    return;
                }
                continue_discovery(flow, std::move(candidates), index + 1, std::move(failure));
            });
}

void on_protected_resource(const FlowPtr& flow, std::expected<McpResponse, Error> response) {
    if (!response) {
        settle(flow, std::unexpected(response.error()));
        return;
    }
    auto document = read_json_response(*response, "the protected resource metadata");
    if (!document) {
        settle(flow, std::unexpected(document.error()));
        return;
    }
    const auto issuers = oauth::string_array_member(*document, "authorization_servers");
    std::vector<DiscoveryStep> candidates;
    for (const auto& issuer : issuers) {
        if (issuer.empty()) {
            continue;
        }
        const auto base = trim_trailing_slashes(issuer);
        candidates.push_back(
                DiscoveryStep{.url = base + std::string{kAuthorizationServerMetadataPath}, .issuer = issuer});
        candidates.push_back(DiscoveryStep{.url = base + std::string{kOpenIdConfigurationPath}, .issuer = issuer});
    }
    if (candidates.empty()) {
        settle(flow,
                std::unexpected(oauth_error("the Upstream MCP Server declared no authorization server",
                        "the protected resource metadata lists no non-empty \"authorization_servers\" entry")));
        return;
    }
    if (const auto scopes = oauth::string_array_member(*document, "scopes_supported"); !scopes.empty()) {
        flow->metadata.scopes = scopes;
    }
    continue_discovery(flow, std::move(candidates), 0, std::nullopt);
}

void discover_metadata(const FlowPtr& flow) {
    const auto origin = oauth::origin_of(flow->request.resource_url);
    if (origin.empty()) {
        settle(flow,
                std::unexpected(oauth_error("the Upstream MCP Server's endpoint is not an absolute URL",
                        "discovery needs the origin of the configured endpoint")));
        return;
    }
    std::string metadata_url = origin + std::string{kProtectedResourceMetadataPath};
    if (flow->request.challenge.has_value() && !flow->request.challenge->resource_metadata_url.empty()) {
        metadata_url = flow->request.challenge->resource_metadata_url;
    }
    exchange(flow->transport, "GET", std::move(metadata_url), {}, {})
            .start([flow](std::expected<McpResponse, Error> response) mutable noexcept {
                on_protected_resource(flow, std::move(response));
            });
}

void resolve_metadata(const FlowPtr& flow) {
    if (!flow->request.metadata.has_value()) {
        discover_metadata(flow);
        return;
    }
    flow->metadata = *flow->request.metadata;
    if (flow->metadata.issuer.empty() || flow->metadata.authorization_endpoint.empty() ||
            flow->metadata.token_endpoint.empty()) {
        settle(flow,
                std::unexpected(oauth_error("the supplied authorization server metadata is incomplete",
                        "the metadata declares no \"issuer\", \"authorization_endpoint\", or \"token_endpoint\"")));
        return;
    }
    register_client(flow);
}

// ── dynamic client registration ───────────────────────────────────────────────

void on_registration(const FlowPtr& flow, std::expected<JsonValue, Error> document) {
    if (!document) {
        settle(flow, std::unexpected(document.error()));
        return;
    }
    auto client_id = oauth::string_member(*document, "client_id");
    if (!client_id.has_value() || client_id->empty()) {
        settle(flow,
                std::unexpected(oauth_error("the authorization server issued no client id",
                        "the client registration response carries no non-empty \"client_id\"")));
        return;
    }
    flow->client_id = std::move(*client_id);
    auto pkce = oauth::generate_pkce();
    auto state_value = oauth::random_url_safe(24);
    if (!pkce.has_value() || !state_value.has_value()) {
        settle(flow,
                std::unexpected(oauth_failure("the authorization request could not be made unpredictable",
                        "the platform random source produced no PKCE verifier or state",
                        {},
                        ErrorCode::Unknown)));
        return;
    }
    flow->verifier = std::move(pkce->verifier);
    flow->code_challenge = std::move(pkce->challenge);
    flow->state = std::move(*state_value);
    flow->redirect_uri =
            "http://127.0.0.1:" + std::to_string(flow->listener->bound_port()) + std::string{kCallbackRoute};
    present_and_wait(flow);
}

void register_new_client(const FlowPtr& flow) {
    if (flow->metadata.registration_endpoint.empty()) {
        settle(flow,
                std::unexpected(
                        oauth_error("the authorization server publishes no dynamic client registration endpoint",
                                "the metadata declares no \"registration_endpoint\", so this client cannot register")));
        return;
    }
    const JsonValue::object_t registration{
            {"client_name", JsonValue{flow->request.client_name}},
            {"redirect_uris", JsonValue{JsonValue::array_t{JsonValue{flow->redirect_uri}}}},
            {"token_endpoint_auth_method", JsonValue{std::string{kTokenEndpointAuthMethod}}},
            {"grant_types", JsonValue{JsonValue::array_t{JsonValue{"authorization_code"}, JsonValue{"refresh_token"}}}},
            {"response_types", JsonValue{JsonValue::array_t{JsonValue{"code"}}}},
            // The 2026-07-28 auth hardening: a client declares what kind of
            // application it is, and a loopback client is `native`.
            {"application_type", JsonValue{std::string{kApplicationType}}},
    };
    auto body = support::write_json(JsonValue{registration});
    if (!body) {
        settle(flow,
                std::unexpected(oauth_error("the dynamic client registration request could not be serialized",
                        "the registration document could not be written")));
        return;
    }
    exchange(flow->transport, "POST", flow->metadata.registration_endpoint, *body, "application/json")
            .start([flow](std::expected<McpResponse, Error> response) mutable noexcept {
                if (!response) {
                    settle(flow, std::unexpected(response.error()));
                    return;
                }
                auto document = read_json_response(*response, "the registration endpoint");
                if (!document) {
                    settle(flow, std::unexpected(document.error()));
                    return;
                }
                on_registration(flow, std::expected<JsonValue, Error>{std::move(*document)});
            });
}

/// The client id this Server Id already registered with this issuer, or a fresh
/// dynamic registration. A second registration would orphan the first client id
/// on the authorization server, so an existing one is always reused.
void on_stored_credential(const FlowPtr& flow, std::expected<std::optional<UpstreamOAuthCredential>, Error> stored) {
    if (!stored) {
        settle(flow,
                std::unexpected(oauth_error("the stored authorization credential could not be read",
                        stored.error().message + " " + stored.error().detail)));
        return;
    }
    if (stored->has_value() && !(*stored)->client_id.empty()) {
        flow->client_id = (*stored)->client_id;
        auto pkce = oauth::generate_pkce();
        auto state_value = oauth::random_url_safe(24);
        if (!pkce.has_value() || !state_value.has_value()) {
            settle(flow,
                    std::unexpected(oauth_failure("the authorization request could not be made unpredictable",
                            "the platform random source produced no PKCE verifier or state",
                            {},
                            ErrorCode::Unknown)));
            return;
        }
        flow->verifier = std::move(pkce->verifier);
        flow->code_challenge = std::move(pkce->challenge);
        flow->state = std::move(*state_value);
        flow->redirect_uri =
                "http://127.0.0.1:" + std::to_string(flow->listener->bound_port()) + std::string{kCallbackRoute};
        present_and_wait(flow);
        return;
    }
    register_new_client(flow);
}

void register_client(const FlowPtr& flow) {
    // A listener that could not bind would leave the redirect URI pointing at
    // nothing, so the flow fails before it asks the user for anything.
    auto listener = oauth::LoopbackCallbackServer::start(oauth::LoopbackCallbackServer::Options{
            .host = "127.0.0.1",
            .port = 0,
            .path = std::string{kCallbackRoute},
    });
    if (!listener.has_value()) {
        settle(flow,
                std::unexpected(oauth_error("the loopback authorization callback could not be started",
                        "no listener could be bound on the loopback interface for the authorization redirect",
                        ErrorCode::Network)));
        return;
    }
    flow->listener = std::move(*listener);
    flow->store->read_oauth(flow->request.server_id, flow->metadata.issuer)
            .start([flow](std::expected<std::optional<UpstreamOAuthCredential>, Error> stored) mutable noexcept {
                on_stored_credential(flow, std::move(stored));
            });
}

// ── the authorization response and the token exchange ────────────────────────

void on_token_response(const FlowPtr& flow, std::expected<JsonValue, Error> document) {
    if (!document) {
        settle(flow, std::unexpected(document.error()));
        return;
    }
    if (document->get_if<JsonValue::object_t>() == nullptr) {
        settle(flow,
                std::unexpected(oauth_error("the authorization server answered with a body that is not an object",
                        "the token response is not a JSON object")));
        return;
    }
    if (const auto reported = oauth::error_of(*document); !reported.empty()) {
        settle(flow, std::unexpected(oauth_error("the authorization server refused the authorization code", reported)));
        return;
    }
    auto access_token = oauth::string_member(*document, "access_token");
    if (!access_token.has_value() || access_token->empty()) {
        settle(flow,
                std::unexpected(oauth_error("the authorization server issued no access token",
                        "the token response carries no non-empty \"access_token\"")));
        return;
    }
    if (const auto token_type = oauth::string_member(*document, "token_type");
            token_type.has_value() && !token_type->empty() && *token_type != "Bearer") {
        settle(flow,
                std::unexpected(oauth_error("the authorization server issued a token type this build cannot use",
                        "the token response declares token_type \"" + *token_type + "\"; only Bearer is used")));
        return;
    }
    UpstreamOAuthCredential credential{
            .issuer = flow->metadata.issuer,
            .client_id = flow->client_id,
            .access_token = *access_token,
    };
    if (const auto refresh = oauth::string_member(*document, "refresh_token");
            refresh.has_value() && !refresh->empty()) {
        credential.refresh_token = *refresh;
    }
    if (const auto expires_in = oauth::integer_member(*document, "expires_in"); expires_in.has_value()) {
        credential.expires_at = now_seconds() + std::max<std::int64_t>(*expires_in, 0);
    }
    if (const auto scope = oauth::string_member(*document, "scope"); scope.has_value() && !scope->empty()) {
        credential.scopes = oauth::split_scopes(*scope);
    } else {
        credential.scopes = requested_scopes(flow->metadata, challenge_scope_of(flow->request));
    }
    // From here the access token is the secret: every failure is redacted
    // against it, so a store failure cannot echo it into a diagnostic.
    const std::string secret = credential.access_token;
    flow->store->write_oauth(flow->request.server_id, credential)
            .start([flow,
                           secret,
                           issuer = credential.issuer,
                           client_id = credential.client_id,
                           access_token = *access_token,
                           refresh = credential.refresh_token,
                           expires_at = credential.expires_at](std::expected<void, Error> written) mutable noexcept {
                if (!written) {
                    settle(flow,
                            std::unexpected(oauth_failure("the authorization credential could not be persisted",
                                    written.error().message + " " + written.error().detail,
                                    secret,
                                    written.error().code)));
                    return;
                }
                UpstreamOAuthGrant grant{
                        .server_id = flow->request.server_id,
                        .issuer = issuer,
                        .client_id = client_id,
                        .expires_at = expires_at,
                        .outcome = UpstreamOAuthOutcome::Authorized,
                };
                if (!refresh.empty()) {
                    grant.refresh_token = refresh;
                }
                settle(flow, Expected<UpstreamOAuthGrant>{std::move(grant)});
            });
}

void exchange_code(const FlowPtr& flow, std::string code) {
    const auto body = oauth::form_encode({
            {"grant_type", "authorization_code"},
            {"code", code},
            {"redirect_uri", flow->redirect_uri},
            {"client_id", flow->client_id},
            {"code_verifier", flow->verifier},
    });
    exchange(flow->transport, "POST", flow->metadata.token_endpoint, body, "application/x-www-form-urlencoded")
            .start([flow, code](std::expected<McpResponse, Error> response) mutable noexcept {
                if (!response) {
                    settle(flow, std::unexpected(response.error()));
                    return;
                }
                auto document = read_json_response(*response, "the token endpoint");
                if (!document) {
                    // The body of a refused exchange can quote the code, so
                    // only the endpoint's own wording is reported.
                    settle(flow,
                            std::unexpected(oauth_failure(
                                    "the authorization code could not be exchanged", document.error().message, code)));
                    return;
                }
                on_token_response(flow, std::expected<JsonValue, Error>{std::move(*document)});
            });
}

/// The authorization response the browser delivered, reduced to the parameters
/// this flow acts on. `iss` is validated here, against the issuer the request
/// was actually sent to, and a mismatch is a hard failure: the code is refused
/// before it is exchanged and nothing is persisted, which is what makes this
/// an authorization-code-injection defence rather than a diagnostic.
[[nodiscard]] Expected<std::string> read_authorization_code(
        const oauth::CallbackRequest& callback, const std::string& expected_state, const std::string& expected_issuer) {
    const auto query = oauth::parse_query(callback.query);
    const auto code = oauth::query_value(query, "code");
    const auto state = oauth::query_value(query, "state");
    const auto issuer = oauth::query_value(query, "iss");
    if (state != expected_state) {
        return std::unexpected(oauth_error("the authorization response did not echo this request's state",
                "the \"state\" parameter does not match the state this client sent"));
    }
    // RFC 9207 §2.4: the client compares the returned `iss` with the issuer it
    // sent the request to.
    if (issuer.empty()) {
        return std::unexpected(oauth_error("the authorization response carried no issuer",
                "the response has no \"iss\" parameter, so the authorization server that issued the code is unknown"));
    }
    if (issuer != expected_issuer) {
        return std::unexpected(oauth_error("the authorization response came from a different authorization server",
                "the response declares the issuer \"" + issuer + "\" but the request was sent to \"" + expected_issuer +
                        "\""));
    }
    if (code.empty()) {
        return std::unexpected(oauth_error("the authorization response carried no authorization code",
                "the response has no non-empty \"code\" parameter"));
    }
    return Expected<std::string>{code};
}

void on_callback(const FlowPtr& flow, oauth::CallbackRequest callback) {
    auto code = read_authorization_code(callback, flow->state, flow->metadata.issuer);
    if (!code) {
        settle(flow, std::unexpected(code.error()));
        return;
    }
    exchange_code(flow, std::move(*code));
}

// ── the browser flow ─────────────────────────────────────────────────────────

void present_and_wait(const FlowPtr& flow) {
    auto parameters = std::vector<std::pair<std::string, std::string>>{
            {"response_type", "code"},
            {"client_id", flow->client_id},
            {"redirect_uri", flow->redirect_uri},
            {"state", flow->state},
            {"code_challenge", flow->code_challenge},
            {"code_challenge_method", "S256"},
    };
    if (!flow->request.resource_url.empty()) {
        // RFC 8707 resource binding: the token the Upstream will accept is
        // bound to the resource it was requested for.
        parameters.emplace_back("resource", flow->request.resource_url);
    }
    if (const auto scopes = requested_scopes(flow->metadata, challenge_scope_of(flow->request)); !scopes.empty()) {
        parameters.emplace_back("scope", join_scopes(scopes));
    }
    const UpstreamOAuthPrompt prompt{
            .server_id = flow->request.server_id,
            .issuer = flow->metadata.issuer,
            .authorization_url = oauth::append_query(flow->metadata.authorization_endpoint, parameters),
    };

    auto once = std::make_shared<Once>();
    // The callback wait is armed *before* the URL is presented: a browser can
    // redirect faster than the flow gets from presenting to waiting, and an
    // unarmed wait would drop a redirect that already happened.
    auto callback_wait = flow->listener->wait({});
    callback_wait.start([flow, once](std::expected<oauth::CallbackRequest, Error> callback) mutable noexcept {
        once->run([flow, outcome = std::move(callback)]() mutable {
            if (!outcome) {
                settle(flow, std::unexpected(outcome.error()));
                return;
            }
            on_callback(flow, std::move(*outcome));
        });
    });
    // Either answer ends the authorization: a dismissal is a cancellation, and
    // a failure to present is a failure. There is no outcome in which the
    // prompt is answered and the flow keeps waiting.
    flow->prompter->present(prompt, {}).start([once, flow](std::expected<void, Error>) mutable noexcept {
        once->run([flow] { settle(flow, std::unexpected(cancelled_error())); });
    });
}

} // namespace

AsyncResult<UpstreamOAuthGrant> authorize_upstream(UpstreamOAuthRequest request,
        std::shared_ptr<McpTransport> transport,
        std::shared_ptr<UpstreamCredentialStore> store,
        std::shared_ptr<UpstreamOAuthPrompter> prompter,
        std::stop_token stop_token) {
    if (stop_token.stop_requested()) {
        return AsyncResult<UpstreamOAuthGrant>(std::unexpected(cancelled_error()));
    }
    if (request.server_id.empty()) {
        return AsyncResult<UpstreamOAuthGrant>(std::unexpected(
                oauth_error("the authorization named no Upstream MCP Server", "the Server Id is empty")));
    }
    if (transport == nullptr) {
        return AsyncResult<UpstreamOAuthGrant>(std::unexpected(oauth_error("the MCP Host has no transport",
                "the authorization needs the one transport seam to reach the authorization server",
                ErrorCode::Network)));
    }
    if (store == nullptr) {
        return AsyncResult<UpstreamOAuthGrant>(std::unexpected(oauth_error("the MCP Host has no credential store",
                "the authorization needs a store to persist the credential under")));
    }
    // A session with nothing to show the URL in cannot authorize anything, and
    // must not wait for a browser nobody is watching: the flow fails closed
    // rather than blocking a non-interactive session.
    if (prompter == nullptr) {
        return AsyncResult<UpstreamOAuthGrant>(
                std::unexpected(oauth_error("this session cannot ask the user to authorize an Upstream MCP Server",
                        "no frontend is installed to present the authorization URL, so the flow cannot start")));
    }
    auto flow = std::make_shared<Flow>();
    flow->request = std::move(request);
    flow->transport = std::move(transport);
    flow->store = std::move(store);
    flow->prompter = std::move(prompter);
    return AsyncResult<UpstreamOAuthGrant>(AsyncResult<UpstreamOAuthGrant>::producer_type(
            [flow, stop_token](AsyncCompletion<UpstreamOAuthGrant, Error> completion) mutable noexcept {
                flow->completion = std::move(completion);
                const std::weak_ptr<Flow> weak = flow;
                flow->stop_watcher.emplace(stop_token, [weak] {
                    if (const auto live = weak.lock()) {
                        settle(live, std::unexpected(cancelled_error()));
                    }
                });
                if (stop_token.stop_requested()) {
                    return settle(flow, std::unexpected(cancelled_error()));
                }
                resolve_metadata(flow);
            }));
}

} // namespace cch::mcp
