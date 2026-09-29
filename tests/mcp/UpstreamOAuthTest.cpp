#include <cch/mcp/UpstreamAuth.hpp>
#include <cch/mcp/UpstreamOAuth.hpp>

#include "mcp/OAuthCallbackServer.hpp"
#include "mcp/OAuthSupport.hpp"
#include "mcp/Protocol.hpp"
#include "support/ScriptedMcpTransport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "support/Json.hpp"

namespace {

using cch::mcp::UpstreamOAuthCredential;
using cch::mcp::UpstreamOAuthGrant;
using cch::mcp::UpstreamOAuthOutcome;
using cch::mcp::UpstreamOAuthPrompt;
using cch::mcp::UpstreamOAuthReport;
using cch::mcp::UpstreamOAuthRequest;
using cch::support::AsyncCompletion;
using cch::support::AsyncResult;
using cch::support::Error;
using cch::support::ErrorCode;
using cch::support::Expected;
using cch::tests::ScriptedMcpAnswer;
using cch::tests::ScriptedMcpTransport;
using JsonValue = cch::support::JsonValue;
using namespace std::chrono_literals;

constexpr std::string_view kIssuer{"https://auth.example.com"};
constexpr std::string_view kResource{"https://mcp.example.com/mcp"};
constexpr std::string_view kPrmUrl{"https://mcp.example.com/.well-known/oauth-protected-resource"};
constexpr std::string_view kAsMetadataUrl{"https://auth.example.com/.well-known/oauth-authorization-server"};
constexpr std::string_view kAuthorizeUrl{"https://auth.example.com/authorize"};
constexpr std::string_view kTokenUrl{"https://auth.example.com/token"};
constexpr std::string_view kRegisterUrl{"https://auth.example.com/register"};
constexpr std::string_view kLoopbackPrefix{"http://127.0.0.1:"};

/// The credential store the OAuth flow resolves and writes through, in memory.
/// It implements the whole `UpstreamCredentialStore` contract — including the
/// issuer keying, which is a property of the contract rather than of this fake.
class MemoryMcpStore final : public cch::mcp::UpstreamCredentialStore {
public:
    [[nodiscard]] AsyncResult<std::optional<std::string>> read_bearer(std::string) override {
        return AsyncResult<std::optional<std::string>>(
                Expected<std::optional<std::string>>{std::optional<std::string>{}});
    }

    [[nodiscard]] AsyncResult<void> write_bearer(std::string, std::string) override {
        return AsyncResult<void>(Expected<void>{});
    }

    [[nodiscard]] AsyncResult<std::optional<UpstreamOAuthCredential>> read_oauth(
            std::string server_id, std::string issuer) override {
        reads.emplace_back(server_id, issuer);
        const auto found = credentials.find(server_id);
        if (found == credentials.end() || found->second.issuer != issuer) {
            // A credential issued by another issuer is not a credential for
            // this one: it reads as absent rather than as a usable token.
            return AsyncResult<std::optional<UpstreamOAuthCredential>>(
                    Expected<std::optional<UpstreamOAuthCredential>>{std::optional<UpstreamOAuthCredential>{}});
        }
        return AsyncResult<std::optional<UpstreamOAuthCredential>>(
                Expected<std::optional<UpstreamOAuthCredential>>{found->second});
    }

    [[nodiscard]] AsyncResult<void> write_oauth(
            std::string server_id, const UpstreamOAuthCredential& credential) override {
        writes.push_back(credential);
        if (!fail_write_with.empty()) {
            return AsyncResult<void>(
                    std::unexpected(cch::support::make_error(ErrorCode::Auth, fail_write_with, fail_write_detail)));
        }
        credentials[server_id] = credential;
        return AsyncResult<void>(Expected<void>{});
    }

    [[nodiscard]] const UpstreamOAuthCredential* credential(std::string_view server_id) const {
        const auto found = credentials.find(std::string{server_id});
        return found == credentials.end() ? nullptr : &found->second;
    }

    std::map<std::string, UpstreamOAuthCredential> credentials{};
    std::vector<UpstreamOAuthCredential> writes{};
    std::vector<std::pair<std::string, std::string>> reads{};
    /// When set, `write_oauth` fails with this message and detail. The detail
    /// stands in for whatever a real store puts in its failure text.
    std::string fail_write_with{};
    std::string fail_write_detail{};
};

/// The prompt port a test drives by hand. `present` stays pending until the
/// test dismisses it or the flow reports its outcome, which is exactly what a
/// frontend does with a dialog it is showing.
class ManualOAuthPrompter final : public cch::mcp::UpstreamOAuthPrompter {
public:
    [[nodiscard]] AsyncResult<void> present(UpstreamOAuthPrompt prompt, std::stop_token) override {
        last_prompt = std::move(prompt);
        presented.store(true, std::memory_order_release);
        if (on_present) {
            on_present(last_prompt);
        }
        return AsyncResult<void>(
                AsyncResult<void>::producer_type([this](AsyncCompletion<void, Error> completion) noexcept {
                    // A dismissal asked for before the operation started is answered
                    // the moment it does: the user closed the dialog before the flow
                    // was waiting on it.
                    if (dismissed.load(std::memory_order_acquire)) {
                        return completion(std::unexpected(
                                cch::support::make_error(ErrorCode::Cancelled, "the authorization was dismissed")));
                    }
                    pending_ = std::move(completion);
                }));
    }

    void finish(UpstreamOAuthReport value) override {
        report = std::move(value);
        finished.store(true, std::memory_order_release);
    }

    /// Answer the outstanding prompt as a dismissal, which is how a user
    /// closes the dialog without authorizing anything.
    void dismiss() {
        dismissed.store(true, std::memory_order_release);
        auto pending = std::move(pending_);
        if (pending) {
            pending(std::unexpected(cch::support::make_error(ErrorCode::Cancelled, "the authorization was dismissed")));
        }
    }

    std::function<void(const UpstreamOAuthPrompt&)> on_present{};
    UpstreamOAuthPrompt last_prompt{};
    std::optional<UpstreamOAuthReport> report{};
    std::atomic_bool presented{false};
    std::atomic_bool finished{false};
    std::atomic_bool dismissed{false};
    AsyncCompletion<void, Error> pending_{};
};

[[nodiscard]] std::string json_text(const JsonValue& value) {
    auto text = cch::support::write_json(value);
    return text ? *text : std::string{};
}

[[nodiscard]] JsonValue json_object(std::initializer_list<std::pair<const std::string, JsonValue>> members) {
    return JsonValue{JsonValue::object_t{members}};
}

/// The query of an absolute URL, as the flow wrote it.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> query_of(std::string_view url) {
    const auto separator = url.find('?');
    if (separator == std::string_view::npos) {
        return {};
    }
    return cch::mcp::oauth::parse_query(url.substr(separator + 1));
}

[[nodiscard]] std::string param_of(const UpstreamOAuthPrompt& prompt, std::string_view name) {
    return cch::mcp::oauth::query_value(query_of(prompt.authorization_url), name);
}

void script_authorization_server(ScriptedMcpTransport& script, std::string_view issuer = kIssuer) {
    script.answer_url(kPrmUrl,
            ScriptedMcpAnswer{
                    .raw_body = json_text(json_object({
                            {"resource", "https://mcp.example.com"},
                            {"authorization_servers", JsonValue{JsonValue::array_t{JsonValue{std::string{issuer}}}}},
                            {"scopes_supported", JsonValue{JsonValue::array_t{JsonValue{"mcp:tools"}}}},
                    }))});
    script.answer_url(kAsMetadataUrl,
            ScriptedMcpAnswer{.raw_body = json_text(json_object({
                                      {"issuer", std::string{issuer}},
                                      {"authorization_endpoint", std::string{kAuthorizeUrl}},
                                      {"token_endpoint", std::string{kTokenUrl}},
                                      {"registration_endpoint", std::string{kRegisterUrl}},
                                      {"scopes_supported", JsonValue{JsonValue::array_t{JsonValue{"mcp:tools"}}}},
                              }))});
    script.answer_url(
            kRegisterUrl, ScriptedMcpAnswer{.raw_body = json_text(json_object({{"client_id", "client-abc"}}))});
}

[[nodiscard]] std::string token_response_body(std::string_view access, std::string_view refresh) {
    return json_text(json_object({
            {"access_token", std::string{access}},
            {"refresh_token", std::string{refresh}},
            {"token_type", "Bearer"},
            {"expires_in", 3600.0},
            {"scope", "mcp:tools"},
    }));
}

/// The loopback endpoint of the authorization request the flow presented.
struct Loopback {
    std::uint16_t port{0};
    std::string path{};
};

[[nodiscard]] Loopback loopback_of(const UpstreamOAuthPrompt& prompt) {
    const auto location = param_of(prompt, "redirect_uri");
    const auto path_start = location.find('/', kLoopbackPrefix.size());
    return Loopback{
            .port = static_cast<std::uint16_t>(
                    std::stoi(location.substr(kLoopbackPrefix.size(), path_start - kLoopbackPrefix.size()))),
            .path = location.substr(path_start),
    };
}

/// Drive one loopback redirect the way a browser would, from a thread of its
/// own: the flow is presenting the URL on the thread that is waiting for this
/// operation, so the redirect may not be written from there.
void follow_redirect(Loopback loopback, std::string query) {
    std::thread([loopback, query = std::move(query)] {
        boost::asio::io_context io;
        boost::asio::ip::tcp::socket socket{io};
        boost::system::error_code error;
        // The listener is bound before the URL is presented, so the first
        // connect answers; the retry covers the window in which a test run
        // under load has not been scheduled yet.
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        do {
            socket.connect({boost::asio::ip::make_address("127.0.0.1"), loopback.port}, error);
            if (!error) {
                break;
            }
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < deadline);
        if (error) {
            return;
        }
        const std::string request =
                "GET " + loopback.path + query + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        boost::asio::write(socket, boost::asio::buffer(request), error);
        std::string response;
        boost::asio::read(socket, boost::asio::dynamic_buffer(response), error);
    }).detach();
}

/// Redirect with a `code` this test chose and the `state` the flow minted.
void redirect_with_code(const UpstreamOAuthPrompt& prompt, std::string_view code, std::string_view issuer) {
    follow_redirect(loopback_of(prompt),
            "?code=" + std::string{code} + "&state=" + param_of(prompt, "state") +
                    "&iss=" + cch::mcp::oauth::url_encode(issuer));
}

template <typename Predicate> void wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(predicate());
}

[[nodiscard]] UpstreamOAuthRequest executor_request() {
    return UpstreamOAuthRequest{.server_id = "executor", .resource_url = std::string{kResource}};
}

} // namespace

TEST_CASE("the full chain discovers, registers, redirects, exchanges, and stores", "[mcp][oauth]") {
    auto transport = std::make_shared<ScriptedMcpTransport>();
    script_authorization_server(*transport);
    (*transport).answer_url(kTokenUrl, ScriptedMcpAnswer{.raw_body = token_response_body("access-1", "refresh-1")});
    auto store = std::make_shared<MemoryMcpStore>();
    auto prompter = std::make_shared<ManualOAuthPrompter>();
    std::optional<Expected<UpstreamOAuthGrant>> outcome;
    prompter->on_present = [](const UpstreamOAuthPrompt& prompt) { redirect_with_code(prompt, "code-1", kIssuer); };

    cch::mcp::authorize_upstream(executor_request(), transport, store, prompter)
            .start([&outcome](std::expected<UpstreamOAuthGrant, Error> result) mutable noexcept {
                outcome = std::move(result);
            });

    wait_until([&outcome] { return outcome.has_value(); });
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->has_value());
    const UpstreamOAuthGrant& grant = **outcome;
    CHECK(grant.issuer == kIssuer);
    CHECK(grant.client_id == "client-abc");
    CHECK(grant.outcome == UpstreamOAuthOutcome::Authorized);
    REQUIRE(grant.refresh_token.has_value());
    CHECK(*grant.refresh_token == "refresh-1");

    // The presented URL is the authorization request, PKCE and resource
    // included, and it carries no secret.
    REQUIRE(prompter->last_prompt.issuer == kIssuer);
    CHECK(param_of(prompter->last_prompt, "response_type") == "code");
    CHECK(param_of(prompter->last_prompt, "client_id") == "client-abc");
    CHECK(param_of(prompter->last_prompt, "code_challenge_method") == "S256");
    CHECK_FALSE(param_of(prompter->last_prompt, "code_challenge").empty());
    CHECK_FALSE(param_of(prompter->last_prompt, "state").empty());
    CHECK(param_of(prompter->last_prompt, "resource") == kResource);
    CHECK(param_of(prompter->last_prompt, "scope") == "mcp:tools");
    CHECK(param_of(prompter->last_prompt, "redirect_uri").starts_with(kLoopbackPrefix));

    // Dynamic client registration declares the application type.
    bool saw_registration = false;
    for (const auto& request : (*transport).requests()) {
        if (request.url != kRegisterUrl) {
            continue;
        }
        saw_registration = true;
        const auto body = cch::support::read_json(request.body);
        REQUIRE(body.has_value());
        CHECK(cch::mcp::oauth::string_member(*body, "application_type") == std::optional<std::string>{"native"});
        CHECK(cch::mcp::oauth::string_member(*body, "token_endpoint_auth_method") ==
                std::optional<std::string>{"none"});
    }
    CHECK(saw_registration);

    // The code exchange is a form-encoded grant carrying the PKCE verifier.
    bool saw_exchange = false;
    for (const auto& request : (*transport).requests()) {
        if (request.url != kTokenUrl) {
            continue;
        }
        saw_exchange = true;
        const auto form = cch::mcp::oauth::parse_query(request.body);
        CHECK(cch::mcp::oauth::query_value(form, "grant_type") == "authorization_code");
        CHECK(cch::mcp::oauth::query_value(form, "code") == "code-1");
        CHECK_FALSE(cch::mcp::oauth::query_value(form, "code_verifier").empty());
        CHECK(request.headers.at("Content-Type") == "application/x-www-form-urlencoded");
    }
    CHECK(saw_exchange);

    // The credential is stored under the issuer that issued it, and the report
    // the frontend reads carries no token.
    REQUIRE(store->writes.size() == 1);
    CHECK(store->writes.front().issuer == kIssuer);
    CHECK(store->writes.front().access_token == "access-1");
    CHECK(store->writes.front().client_id == "client-abc");
    REQUIRE(prompter->report.has_value());
    CHECK(prompter->report->outcome == UpstreamOAuthOutcome::Authorized);
    CHECK(prompter->report->message.find("access-1") == std::string::npos);
}

TEST_CASE("an authorization response from another issuer is refused and nothing is persisted", "[mcp][oauth]") {
    auto transport = std::make_shared<ScriptedMcpTransport>();
    script_authorization_server(*transport);
    (*transport).answer_url(kTokenUrl, ScriptedMcpAnswer{.raw_body = token_response_body("access-1", "refresh-1")});
    auto store = std::make_shared<MemoryMcpStore>();
    auto prompter = std::make_shared<ManualOAuthPrompter>();
    std::optional<Expected<UpstreamOAuthGrant>> outcome;
    prompter->on_present = [](const UpstreamOAuthPrompt& prompt) {
        redirect_with_code(prompt, "injected", "https://evil.example.com");
    };

    cch::mcp::authorize_upstream(executor_request(), transport, store, prompter)
            .start([&outcome](std::expected<UpstreamOAuthGrant, Error> result) mutable noexcept {
                outcome = std::move(result);
            });

    wait_until([&outcome] { return outcome.has_value(); });
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->has_value());
    CHECK(outcome->error().code == ErrorCode::OAuth);
    // The code is refused before it is exchanged: no token request was made.
    for (const auto& request : (*transport).requests()) {
        CHECK(request.url != kTokenUrl);
    }
    CHECK(store->writes.empty());
    CHECK(store->credential("executor") == nullptr);
    REQUIRE(prompter->report.has_value());
    CHECK(prompter->report->outcome == UpstreamOAuthOutcome::Failed);
}

TEST_CASE("an authorization response with no issuer is refused", "[mcp][oauth]") {
    auto transport = std::make_shared<ScriptedMcpTransport>();
    script_authorization_server(*transport);
    auto store = std::make_shared<MemoryMcpStore>();
    auto prompter = std::make_shared<ManualOAuthPrompter>();
    std::optional<Expected<UpstreamOAuthGrant>> outcome;
    prompter->on_present = [](const UpstreamOAuthPrompt& prompt) {
        follow_redirect(loopback_of(prompt), "?code=c&state=" + param_of(prompt, "state"));
    };

    cch::mcp::authorize_upstream(executor_request(), transport, store, prompter)
            .start([&outcome](std::expected<UpstreamOAuthGrant, Error> result) mutable noexcept {
                outcome = std::move(result);
            });

    wait_until([&outcome] { return outcome.has_value(); });
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->has_value());
    CHECK(outcome->error().code == ErrorCode::OAuth);
    CHECK(store->writes.empty());
}

TEST_CASE("a response that does not echo the state is refused before the code is exchanged", "[mcp][oauth]") {
    auto transport = std::make_shared<ScriptedMcpTransport>();
    script_authorization_server(*transport);
    auto store = std::make_shared<MemoryMcpStore>();
    auto prompter = std::make_shared<ManualOAuthPrompter>();
    std::optional<Expected<UpstreamOAuthGrant>> outcome;
    prompter->on_present = [](const UpstreamOAuthPrompt& prompt) {
        follow_redirect(loopback_of(prompt), "?code=c&state=not-the-state&iss=" + std::string{kIssuer});
    };

    cch::mcp::authorize_upstream(executor_request(), transport, store, prompter)
            .start([&outcome](std::expected<UpstreamOAuthGrant, Error> result) mutable noexcept {
                outcome = std::move(result);
            });

    wait_until([&outcome] { return outcome.has_value(); });
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->has_value());
    for (const auto& request : (*transport).requests()) {
        CHECK(request.url != kTokenUrl);
    }
    CHECK(store->writes.empty());
}

TEST_CASE("a dismissed authorization persists nothing and completes as cancelled", "[mcp][oauth]") {
    auto transport = std::make_shared<ScriptedMcpTransport>();
    script_authorization_server(*transport);
    auto store = std::make_shared<MemoryMcpStore>();
    auto prompter = std::make_shared<ManualOAuthPrompter>();
    std::optional<Expected<UpstreamOAuthGrant>> outcome;
    prompter->on_present = [&prompter](const UpstreamOAuthPrompt&) { prompter->dismiss(); };

    cch::mcp::authorize_upstream(executor_request(), transport, store, prompter)
            .start([&outcome](std::expected<UpstreamOAuthGrant, Error> result) mutable noexcept {
                outcome = std::move(result);
            });

    wait_until([&outcome] { return outcome.has_value(); });
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->has_value());
    CHECK(outcome->error().code == ErrorCode::Cancelled);
    CHECK(store->writes.empty());
    REQUIRE(prompter->report.has_value());
    CHECK(prompter->report->outcome == UpstreamOAuthOutcome::Cancelled);
}

TEST_CASE("a session with no prompt port fails closed without contacting the authorization server", "[mcp][oauth]") {
    auto transport = std::make_shared<ScriptedMcpTransport>();
    script_authorization_server(*transport);
    auto store = std::make_shared<MemoryMcpStore>();
    std::optional<Expected<UpstreamOAuthGrant>> outcome;
    cch::mcp::authorize_upstream(executor_request(), transport, store, nullptr)
            .start([&outcome](std::expected<UpstreamOAuthGrant, Error> result) mutable noexcept {
                outcome = std::move(result);
            });
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->has_value());
    CHECK((*transport).request_count() == 0);
    CHECK(store->writes.empty());
}

TEST_CASE("a store failure cannot echo the issued token into the flow's error", "[mcp][oauth]") {
    auto transport = std::make_shared<ScriptedMcpTransport>();
    script_authorization_server(*transport);
    (*transport).answer_url(kTokenUrl, ScriptedMcpAnswer{.raw_body = token_response_body("super-secret-token", "r")});
    auto store = std::make_shared<MemoryMcpStore>();
    store->fail_write_with = "the credential store rejected the write";
    store->fail_write_detail = "the write it rejected mentioned super-secret-token";
    auto prompter = std::make_shared<ManualOAuthPrompter>();
    std::optional<Expected<UpstreamOAuthGrant>> outcome;
    prompter->on_present = [](const UpstreamOAuthPrompt& prompt) { redirect_with_code(prompt, "c", kIssuer); };

    cch::mcp::authorize_upstream(executor_request(), transport, store, prompter)
            .start([&outcome](std::expected<UpstreamOAuthGrant, Error> result) mutable noexcept {
                outcome = std::move(result);
            });

    wait_until([&outcome] { return outcome.has_value(); });
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->has_value());
    const std::string reported = outcome->error().message + " " + outcome->error().detail;
    CHECK(reported.find("super-secret-token") == std::string::npos);
    CHECK(store->credential("executor") == nullptr);
}

TEST_CASE("an authorization server with no registration endpoint fails rather than guessing a client id",
        "[mcp][oauth]") {
    auto transport = std::make_shared<ScriptedMcpTransport>();
    (*transport)
            .answer_url(kPrmUrl,
                    ScriptedMcpAnswer{.raw_body = json_text(json_object({{"authorization_servers",
                                              JsonValue{JsonValue::array_t{JsonValue{std::string{kIssuer}}}}}}))});
    (*transport)
            .answer_url(kAsMetadataUrl,
                    ScriptedMcpAnswer{.raw_body = json_text(json_object({
                                              {"issuer", std::string{kIssuer}},
                                              {"authorization_endpoint", std::string{kAuthorizeUrl}},
                                              {"token_endpoint", std::string{kTokenUrl}},
                                      }))});
    auto store = std::make_shared<MemoryMcpStore>();
    auto prompter = std::make_shared<ManualOAuthPrompter>();
    std::optional<Expected<UpstreamOAuthGrant>> outcome;
    cch::mcp::authorize_upstream(executor_request(), transport, store, prompter)
            .start([&outcome](std::expected<UpstreamOAuthGrant, Error> result) mutable noexcept {
                outcome = std::move(result);
            });
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->has_value());
    CHECK(outcome->error().code == ErrorCode::OAuth);
    CHECK_FALSE(prompter->presented.load());
    CHECK(store->writes.empty());
}

TEST_CASE("a client this Server Id already registered with is reused rather than registered again", "[mcp][oauth]") {
    auto transport = std::make_shared<ScriptedMcpTransport>();
    script_authorization_server(*transport);
    (*transport).answer_url(kTokenUrl, ScriptedMcpAnswer{.raw_body = token_response_body("access-2", "refresh-2")});
    auto store = std::make_shared<MemoryMcpStore>();
    store->credentials["executor"] = UpstreamOAuthCredential{
            .issuer = std::string{kIssuer},
            .client_id = "client-existing",
            .access_token = "stale",
    };
    auto prompter = std::make_shared<ManualOAuthPrompter>();
    std::optional<Expected<UpstreamOAuthGrant>> outcome;
    prompter->on_present = [](const UpstreamOAuthPrompt& prompt) { redirect_with_code(prompt, "c2", kIssuer); };

    cch::mcp::authorize_upstream(executor_request(), transport, store, prompter)
            .start([&outcome](std::expected<UpstreamOAuthGrant, Error> result) mutable noexcept {
                outcome = std::move(result);
            });

    wait_until([&outcome] { return outcome.has_value(); });
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->has_value());
    const UpstreamOAuthGrant& reused = **outcome;
    CHECK(reused.client_id == "client-existing");
    for (const auto& request : (*transport).requests()) {
        CHECK(request.url != kRegisterUrl);
    }
}

TEST_CASE("an OAuth credential is never presented to an issuer that did not issue it", "[mcp][oauth]") {
    auto store = std::make_shared<MemoryMcpStore>();
    store->credentials["executor"] = UpstreamOAuthCredential{
            .issuer = "https://other.example.com",
            .client_id = "client-other",
            .access_token = "not-ours",
    };
    std::optional<Expected<cch::mcp::UpstreamAuth>> outcome;
    cch::mcp::resolve_oauth_bearer("executor", std::string{kIssuer}, store)
            .start([&outcome](std::expected<cch::mcp::UpstreamAuth, Error> result) mutable noexcept {
                outcome = std::move(result);
            });
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->has_value());
    CHECK_FALSE((*outcome)->bearer.has_value());
}

TEST_CASE("the loopback listener answers the redirect and hands the flow its query", "[mcp][oauth]") {
    auto bound = cch::mcp::oauth::LoopbackCallbackServer::start({});
    REQUIRE(bound.has_value());
    auto& listener = *bound;
    CHECK(listener->bound_port() != 0);
    std::optional<Expected<cch::mcp::oauth::CallbackRequest>> outcome;
    listener->wait({}).start(
            [&outcome](std::expected<cch::mcp::oauth::CallbackRequest, Error> result) mutable noexcept {
                outcome = std::move(result);
            });

    boost::asio::io_context io;
    boost::asio::ip::tcp::socket socket{io};
    boost::system::error_code error;
    socket.connect({boost::asio::ip::make_address("127.0.0.1"), listener->bound_port()}, error);
    REQUIRE_FALSE(error);
    const std::string request =
            "GET /callback?code=abc&iss=" + std::string{kIssuer} + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(request), error);
    REQUIRE_FALSE(error);
    std::string response;
    boost::asio::read(socket, boost::asio::dynamic_buffer(response), error);

    wait_until([&outcome] { return outcome.has_value(); });
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->has_value());
    CHECK((*outcome)->path == "/callback");
    CHECK(cch::mcp::oauth::query_value(cch::mcp::oauth::parse_query((*outcome)->query), "code") == "abc");
    CHECK(response.starts_with("HTTP/1.1 200"));
    listener->close();
}

TEST_CASE("the loopback listener refuses a route it does not serve", "[mcp][oauth]") {
    auto bound = cch::mcp::oauth::LoopbackCallbackServer::start({});
    REQUIRE(bound.has_value());
    auto& listener = *bound;
    std::optional<Expected<cch::mcp::oauth::CallbackRequest>> outcome;
    listener->wait({}).start(
            [&outcome](std::expected<cch::mcp::oauth::CallbackRequest, Error> result) mutable noexcept {
                outcome = std::move(result);
            });

    boost::asio::io_context io;
    boost::asio::ip::tcp::socket socket{io};
    boost::system::error_code error;
    socket.connect({boost::asio::ip::make_address("127.0.0.1"), listener->bound_port()}, error);
    REQUIRE_FALSE(error);
    const std::string request = "GET /somewhere-else?code=abc HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(request), error);
    std::string response;
    boost::asio::read(socket, boost::asio::dynamic_buffer(response), error);

    wait_until([&outcome] { return outcome.has_value(); });
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->has_value());
    CHECK(response.starts_with("HTTP/1.1 404"));
    listener->close();
}

TEST_CASE("a cancelled wait settles rather than blocking the flow", "[mcp][oauth]") {
    auto bound = cch::mcp::oauth::LoopbackCallbackServer::start({});
    REQUIRE(bound.has_value());
    auto& listener = *bound;
    std::stop_source source;
    std::optional<Expected<cch::mcp::oauth::CallbackRequest>> outcome;
    listener->wait(source.get_token())
            .start([&outcome](std::expected<cch::mcp::oauth::CallbackRequest, Error> result) mutable noexcept {
                outcome = std::move(result);
            });
    source.request_stop();
    wait_until([&outcome] { return outcome.has_value(); });
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->has_value());
    CHECK(outcome->error().code == ErrorCode::Cancelled);
    listener->close();
}

TEST_CASE("a loopback issuer is redacted and bounded before it reaches a status surface",
        "[mcp][oauth][redaction][limits][issue849][spec]") {
    auto transport = std::make_shared<ScriptedMcpTransport>();
    script_authorization_server(*transport);
    auto store = std::make_shared<MemoryMcpStore>();
    auto prompter = std::make_shared<ManualOAuthPrompter>();
    std::optional<Expected<UpstreamOAuthGrant>> outcome;
    std::string state;
    prompter->on_present = [&state](const UpstreamOAuthPrompt& prompt) {
        state = param_of(prompt, "state");
        // `iss` is query text from whoever reached the loopback port. It is
        // oversized and it echoes this flow's own state, so a detail that
        // carried it raw would put both an unbounded string and a CSRF value
        // on a status surface.
        const std::string issuer = std::string(4000, 'x') + state + std::string(4000, 'y');
        redirect_with_code(prompt, "code-1", issuer);
    };

    cch::mcp::authorize_upstream(executor_request(), transport, store, prompter)
            .start([&outcome](std::expected<UpstreamOAuthGrant, Error> result) mutable noexcept {
                outcome = std::move(result);
            });

    wait_until([&outcome] { return outcome.has_value(); });
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->has_value());
    CHECK(outcome->error().code == ErrorCode::OAuth);
    const auto& detail = outcome->error().detail;
    CHECK(detail.size() <= cch::mcp::protocol::kMaxDiagnosticBytes);
    CHECK(detail.find(state) == std::string::npos);
    CHECK(store->writes.empty());
}

TEST_CASE("the loopback listener bounds the error text a caller supplied",
        "[mcp][oauth][redaction][limits][issue849][spec]") {
    auto bound = cch::mcp::oauth::LoopbackCallbackServer::start({});
    REQUIRE(bound.has_value());
    auto& listener = *bound;
    std::optional<Expected<cch::mcp::oauth::CallbackRequest>> outcome;
    listener->wait({}).start(
            [&outcome](std::expected<cch::mcp::oauth::CallbackRequest, Error> result) mutable noexcept {
                outcome = std::move(result);
            });

    boost::asio::io_context io;
    boost::asio::ip::tcp::socket socket{io};
    boost::system::error_code error;
    socket.connect({boost::asio::ip::make_address("127.0.0.1"), listener->bound_port()}, error);
    REQUIRE_FALSE(error);
    // `error` is query text from an unauthenticated caller on the loopback
    // port, so it is bounded before it can reach a status surface.
    const std::string request = "GET /callback?error=" + std::string(4000, 'e') +
                                " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(request), error);
    std::string response;
    boost::asio::read(socket, boost::asio::dynamic_buffer(response), error);

    wait_until([&outcome] { return outcome.has_value(); });
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->has_value());
    CHECK(outcome->error().code == ErrorCode::OAuth);
    CHECK(outcome->error().detail.size() <= cch::mcp::protocol::kMaxDiagnosticBytes);
    CHECK(response.starts_with("HTTP/1.1 400"));
    listener->close();
}

TEST_CASE("the loopback listener refuses a request head over the bound it reads",
        "[mcp][oauth][limits][issue849][spec]") {
    auto bound = cch::mcp::oauth::LoopbackCallbackServer::start({});
    REQUIRE(bound.has_value());
    auto& listener = *bound;
    std::optional<Expected<cch::mcp::oauth::CallbackRequest>> outcome;
    listener->wait({}).start(
            [&outcome](std::expected<cch::mcp::oauth::CallbackRequest, Error> result) mutable noexcept {
                outcome = std::move(result);
            });

    boost::asio::io_context io;
    boost::asio::ip::tcp::socket socket{io};
    boost::system::error_code error;
    socket.connect({boost::asio::ip::make_address("127.0.0.1"), listener->bound_port()}, error);
    REQUIRE_FALSE(error);
    // A *complete* request head — blank line included — that is far larger
    // than the bound the listener reads. An unbounded read would accept it and
    // hand the flow a callback; a bounded one refuses it, so this caller
    // cannot decide how much of the flow's memory a callback request occupies.
    const std::string request = "GET /callback?code=abc HTTP/1.1\r\nX-Pad: " + std::string(16 * 1024, 'p') + "\r\n\r\n";
    boost::asio::write(socket, boost::asio::buffer(request), error);
    std::string response;
    boost::asio::read(socket, boost::asio::dynamic_buffer(response), error);

    wait_until([&outcome] { return outcome.has_value(); });
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->has_value());
    CHECK(outcome->error().code == ErrorCode::Network);
    CHECK(outcome->error().detail.find("larger than") != std::string::npos);
    CHECK(response.starts_with("HTTP/1.1 503"));
    listener->close();
}
