#include <cch/ai/Models.hpp>
#include <cch/ai/Provider.hpp>
#include <cch/util/Error.hpp>
#include "ai/ModelStreamBridge.hpp"
#include "ai/providers/EnvApiKeyAuth.hpp"
#include "support/ModelFixture.hpp"
#include "support/StreamAdapterFixture.hpp"
#include "util/ExpectedMacros.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

template <typename T>
T run_awaitable(boost::asio::awaitable<T> operation) {
    boost::asio::io_context io;
    auto result = boost::asio::co_spawn(io, std::move(operation), boost::asio::use_future);
    io.run();
    return result.get();
}

class MemoryCredentialStore final : public ai::CredentialStore {
public:
    [[nodiscard]] cch::support::AsyncResult<std::optional<ai::Credential>> read(
        std::string provider_id) override {
        ++read_count;
        if (throw_read) {
            throw std::runtime_error{"credential store callback threw"};
        }
        std::optional<ai::Credential> value;
        if (const auto found = records.find(provider_id); found != records.end()) {
            value = found->second;
        }
        return cch::support::AsyncResult<std::optional<ai::Credential>>(
            std::expected<std::optional<ai::Credential>, cch::support::Error>{std::move(value)});
    }

    [[nodiscard]] cch::support::AsyncResult<std::vector<ai::CredentialInfo>> list() override {
        return cch::support::AsyncResult<std::vector<ai::CredentialInfo>>(
            std::expected<std::vector<ai::CredentialInfo>, cch::support::Error>{
                std::vector<ai::CredentialInfo>{}});
    }

    [[nodiscard]] cch::support::AsyncResult<std::optional<ai::Credential>> modify(
        std::string provider_id,
        ai::CredentialModifyHook modifier) override {
        ++modify_count;
        if (fail_modify) {
            return cch::support::AsyncResult<std::optional<ai::Credential>>(
                std::expected<std::optional<ai::Credential>, cch::support::Error>{
                    std::unexpect,
                    util::make_error(util::ErrorCode::Unknown, "store write failed")});
        }
        std::optional<ai::Credential> current;
        if (const auto found = records.find(provider_id); found != records.end()) {
            current = found->second;
        }
        auto updated = tests::run_hook(modifier(std::move(current)));
        if (!updated) {
            return cch::support::AsyncResult<std::optional<ai::Credential>>(
                std::expected<std::optional<ai::Credential>, cch::support::Error>{
                    std::unexpect, std::move(updated.error())});
        }
        if (*updated) {
            records.insert_or_assign(provider_id, **updated);
        }
        const auto found = records.find(provider_id);
        if (found == records.end()) {
            return cch::support::AsyncResult<std::optional<ai::Credential>>(
                std::expected<std::optional<ai::Credential>, cch::support::Error>{
                    std::optional<ai::Credential>{}});
        }
        return cch::support::AsyncResult<std::optional<ai::Credential>>(
            std::expected<std::optional<ai::Credential>, cch::support::Error>{
                found->second});
    }

    [[nodiscard]] cch::support::AsyncResult<void> remove(
        std::string provider_id) override {
        ++remove_count;
        records.erase(provider_id);
        return cch::support::AsyncResult<void>(
            std::expected<void, cch::support::Error>{});
    }

    std::map<std::string, ai::Credential, std::less<>> records;
    int read_count{0};
    int modify_count{0};
    int remove_count{0};
    bool throw_read{false};
    bool fail_modify{false};
};

class FakeAuthContext final : public ai::AuthContext {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<std::string>>> environment(
        std::string name) const override {
        const auto found = environment_values.find(name);
        if (found == environment_values.end()) {
            co_return std::optional<std::string>{};
        }
        co_return std::optional<std::string>{found->second};
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<bool>> file_exists(
        std::string) const override {
        co_return false;
    }

    std::map<std::string, std::string, std::less<>> environment_values;
};

ai::ProviderAuth keyless_auth() {
    ai::ApiKeyAuth api_key;
    api_key.name = "keyless";
    api_key.check = [](const ai::AuthContext&, std::optional<ai::ApiKeyCredential>)
        -> boost::asio::awaitable<util::Expected<std::optional<ai::AuthCheck>>> {
        co_return ai::AuthCheck{.source = "keyless", .type = ai::AuthType::ApiKey};
    };
    api_key.resolve = [](const ai::AuthContext&, std::optional<ai::ApiKeyCredential>)
        -> boost::asio::awaitable<util::Expected<std::optional<ai::AuthResult>>> {
        co_return ai::AuthResult{.auth = {}, .env = {}, .source = "keyless"};
    };
    return ai::ProviderAuth{.api_key = std::move(api_key)};
}

class RecordingProvider final : public ai::Provider {
public:
    RecordingProvider(std::string id, ai::ProviderAuth auth = keyless_auth())
        : id_(std::move(id)), auth_(std::move(auth)) {}

    [[nodiscard]] std::string_view id() const noexcept override { return id_; }
    [[nodiscard]] std::string_view name() const noexcept override { return id_; }
    [[nodiscard]] ai::ProviderAuth& auth() noexcept override { return auth_; }
    [[nodiscard]] std::vector<ai::Model> models() const override { return catalog; }

    [[nodiscard]] ai::ModelStream stream(
        ai::Model model,
        ai::AiContext,
        ai::ProviderStreamOptions options) override {
        return ai::detail::make_model_stream(
            [this, model = std::move(model), options = std::move(options)](
                ai::AssistantEventSink sink)
                -> boost::asio::awaitable<util::Expected<ai::AssistantMessage>> {
                seen_models.push_back(model);
                seen_options.push_back(options);
                if (options.stop_token.stop_requested()) {
                    co_return std::unexpected(util::make_error(
                        util::ErrorCode::Cancelled,
                        "provider cancelled"));
                }
                if (throw_stream) {
                    throw std::runtime_error{"provider callback threw"};
                }
                if (stream_failure) {
                    co_return std::unexpected(*stream_failure);
                }
                ai::AssistantMessage message = ai::assistant_text_message("delegated");
                message.api = model.api;
                message.provider = id_;
                message.model = model.id;
                CCH_TRY_VOID(sink(ai::AssistantDoneEvent{
                    .reason = message.stop_reason,
                    .message = message,
                }));
                co_return message;
            });
    }

    std::vector<ai::Model> catalog;
    std::vector<ai::Model> seen_models;
    std::vector<ai::ProviderStreamOptions> seen_options;
    std::optional<util::Error> stream_failure;
    bool throw_stream{false};

private:
    std::string id_;
    ai::ProviderAuth auth_;
};

class ThrowingCatalogProvider final : public ai::Provider {
public:
    [[nodiscard]] std::string_view id() const noexcept override { return "throwing-catalog"; }
    [[nodiscard]] std::string_view name() const noexcept override { return "throwing-catalog"; }
    [[nodiscard]] ai::ProviderAuth& auth() noexcept override { return auth_; }
    [[nodiscard]] std::vector<ai::Model> models() const override {
        throw std::runtime_error{"catalog callback threw"};
    }

    [[nodiscard]] ai::ModelStream stream(
        ai::Model,
        ai::AiContext,
        ai::ProviderStreamOptions) override {
        return ai::detail::make_model_stream(
            [](ai::AssistantEventSink)
                -> boost::asio::awaitable<util::Expected<ai::AssistantMessage>> {
                co_return ai::assistant_text_message("unused");
            });
    }

private:
    ai::ProviderAuth auth_{keyless_auth()};
};

class UnsafeTerminalProvider final : public ai::Provider {
public:
    [[nodiscard]] std::string_view id() const noexcept override { return "unsafe-terminal"; }
    [[nodiscard]] std::string_view name() const noexcept override { return "unsafe-terminal"; }
    [[nodiscard]] ai::ProviderAuth& auth() noexcept override { return auth_; }
    [[nodiscard]] std::vector<ai::Model> models() const override { return {}; }

    [[nodiscard]] ai::ModelStream stream(
        ai::Model model,
        ai::AiContext,
        ai::ProviderStreamOptions) override {
        return ai::detail::make_model_stream(
            [model = std::move(model)](ai::AssistantEventSink sink)
                -> boost::asio::awaitable<util::Expected<ai::AssistantMessage>> {
                const std::string secret = "sk-abcdefghijklmnopqrstuvwxyz1234567890";
                auto message = ai::assistant_text_message("unsafe terminal");
                message.api = model.api;
                message.provider = model.provider;
                message.model = model.id;
                message.stop_reason = ai::AssistantStopReason::Error;
                message.error_message = secret + std::string(2048, 'x');
                CCH_TRY_VOID(sink(ai::AssistantErrorEvent{
                    .reason = ai::AssistantStopReason::Error,
                    .error = message,
                    .failure = util::make_error(
                        util::ErrorCode::Stream,
                        "provider terminal " + secret + std::string(2048, 'x')),
                }));
                co_return std::unexpected(util::make_error(
                    util::ErrorCode::Stream,
                    "provider returned an error after its terminal event"));
            });
    }

private:
    ai::ProviderAuth auth_{keyless_auth()};
};

class DuplicateTerminalProvider final : public ai::Provider {
public:
    [[nodiscard]] std::string_view id() const noexcept override { return "duplicate"; }
    [[nodiscard]] std::string_view name() const noexcept override { return "duplicate"; }
    [[nodiscard]] ai::ProviderAuth& auth() noexcept override { return auth_; }
    [[nodiscard]] std::vector<ai::Model> models() const override { return {}; }

    [[nodiscard]] ai::ModelStream stream(
        ai::Model model,
        ai::AiContext,
        ai::ProviderStreamOptions) override {
        return ai::detail::make_model_stream(
            [model = std::move(model)](ai::AssistantEventSink sink)
                -> boost::asio::awaitable<util::Expected<ai::AssistantMessage>> {
                auto first = ai::assistant_text_message("first terminal");
                first.api = model.api;
                first.provider = model.provider;
                first.model = model.id;
                first.stop_reason = ai::AssistantStopReason::Error;
                first.error_message = "first failure";
                auto second = first;
                second.error_message = "duplicate failure";
                CCH_TRY_VOID(sink(ai::AssistantErrorEvent{
                    .reason = ai::AssistantStopReason::Error,
                    .error = first,
                    .failure = util::make_error(util::ErrorCode::Stream, "first failure"),
                }));
                CCH_TRY_VOID(sink(ai::AssistantErrorEvent{
                    .reason = ai::AssistantStopReason::Error,
                    .error = second,
                    .failure = util::make_error(util::ErrorCode::Stream, "duplicate failure"),
                }));
                co_return std::unexpected(util::make_error(
                    util::ErrorCode::Stream,
                    "provider returned an error after its terminal event"));
            });
    }

private:
    ai::ProviderAuth auth_{keyless_auth()};
};

struct RunResult {
    util::Expected<ai::AssistantMessage> result;
    std::vector<ai::AssistantStreamEvent> events;
};

RunResult run_models(
    std::shared_ptr<ai::Models> models,
    ai::Model model,
    ai::AiContext context = {},
    ai::SimpleStreamOptions options = {},
    bool fail_sink = false,
    util::ErrorCode sink_error_code = util::ErrorCode::Unknown) {
    std::vector<ai::AssistantStreamEvent> events;
    auto stream = models->stream(
        std::move(model), std::move(context), std::move(options));
    auto result = run_awaitable(ai::consume(
        std::move(stream),
        [&](const ai::AssistantStreamEvent& event) -> util::ExpectedVoid {
            events.push_back(event);
            if (fail_sink) {
                return std::unexpected(util::make_error(
                    sink_error_code,
                    "consumer sink failed"));
            }
            return {};
        }));
    return RunResult{
        .result = std::move(result),
        .events = std::move(events),
    };
}

const ai::AssistantErrorEvent& require_terminal_error(const RunResult& run) {
    REQUIRE(run.events.size() == 1);
    const auto* terminal = std::get_if<ai::AssistantErrorEvent>(&run.events.front());
    REQUIRE(terminal != nullptr);
    return *terminal;
}

std::shared_ptr<ai::Models> make_models(
    const std::shared_ptr<MemoryCredentialStore>& credentials,
    const std::shared_ptr<FakeAuthContext>& auth_context) {
    return std::make_shared<ai::Models>(credentials, auth_context);
}


ai::ProviderAuth oauth_login_auth(
    ai::OAuthLoginHook login,
    std::string name = "oauth-provider") {
    ai::OAuthAuth oauth;
    oauth.name = std::move(name);
    oauth.login = std::move(login);
    return ai::ProviderAuth{.oauth = std::move(oauth)};
}

ai::ProviderAuth api_key_login_auth(
    ai::ApiKeyLoginHook login,
    std::string name = "api-key-provider") {
    ai::ApiKeyAuth api_key;
    api_key.name = std::move(name);
    api_key.login = std::move(login);
    return ai::ProviderAuth{.api_key = std::move(api_key)};
}

ai::AuthInteraction empty_interaction() {
    ai::AuthInteraction interaction;
    interaction.notify = [](const ai::AuthEvent&) {};
    return interaction;
}

} // namespace

TEST_CASE("Models selects a long-lived Provider by Model provider identity", "[ai][models][issue338]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    auto auth_context = std::make_shared<FakeAuthContext>();
    auto models = make_models(credentials, auth_context);
    auto first = std::make_shared<RecordingProvider>("first");
    auto second = std::make_shared<RecordingProvider>("second");
    REQUIRE(models->set_provider(first));
    REQUIRE(models->set_provider(second));

    ai::Model request = tests::make_model("chosen-model", "second", "private-api");
    auto run = run_models(models, std::move(request));

    REQUIRE(run.result);
    CHECK(run.result->provider == "second");
    CHECK(first->seen_models.empty());
    REQUIRE(second->seen_models.size() == 1);
    CHECK(second->seen_models.front().api == "private-api");
}

TEST_CASE("Models suppresses throwing Provider catalogs per provider", "[ai][models][issue338]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    auto auth_context = std::make_shared<FakeAuthContext>();
    auto models = make_models(credentials, auth_context);
    auto available = std::make_shared<RecordingProvider>("available");
    available->catalog.push_back(tests::make_model("available-model", "available", "api"));
    REQUIRE(models->set_provider(available));
    REQUIRE(models->set_provider(std::make_shared<ThrowingCatalogProvider>()));

    const auto all = models->models();

    REQUIRE(all.size() == 1);
    CHECK(all.front().id == "available-model");
    CHECK(models->models("throwing-catalog").empty());
    CHECK_FALSE(models->model("throwing-catalog", "missing"));
}

TEST_CASE("Models normalizes provider lookup and model validation failures", "[ai][models][issue338]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    auto auth_context = std::make_shared<FakeAuthContext>();
    auto models = make_models(credentials, auth_context);

    ai::Model missing_request = tests::make_model("model", "missing", "api");
    auto missing = run_models(models, std::move(missing_request));
    REQUIRE(missing.result);
    CHECK(missing.result->stop_reason == ai::AssistantStopReason::Error);
    const auto& missing_terminal = require_terminal_error(missing);
    REQUIRE(missing_terminal.failure);
    CHECK(missing_terminal.failure->code == util::ErrorCode::Provider);
    CHECK(missing_terminal.error.error_message == missing.result->error_message);

    auto provider = std::make_shared<RecordingProvider>("known");
    REQUIRE(models->set_provider(provider));
    ai::Model invalid_request = tests::make_model("", "known", "api");
    auto invalid = run_models(models, std::move(invalid_request));
    REQUIRE(invalid.result);
    const auto& invalid_terminal = require_terminal_error(invalid);
    REQUIRE(invalid_terminal.failure);
    CHECK(invalid_terminal.failure->code == util::ErrorCode::ModelValidation);
    CHECK(provider->seen_models.empty());
}

TEST_CASE("Models applies explicit stored and ambient API key precedence", "[ai][models][auth][issue338]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    ai::ApiKeyCredential stored;
    stored.key = "stored-key";
    credentials->records.emplace("provider", ai::Credential{stored});
    auto auth_context = std::make_shared<FakeAuthContext>();
    auth_context->environment_values.emplace("API_KEY", "ambient-key");

    std::vector<std::string> resolved_keys;
    ai::ApiKeyAuth api_key;
    api_key.name = "key";
    api_key.check = [](const ai::AuthContext&, std::optional<ai::ApiKeyCredential> credential)
        -> boost::asio::awaitable<util::Expected<std::optional<ai::AuthCheck>>> {
        if (!credential || !credential->key) {
            co_return std::optional<ai::AuthCheck>{};
        }
        co_return ai::AuthCheck{.source = "stored", .type = ai::AuthType::ApiKey};
    };
    api_key.resolve = [&resolved_keys](
                          const ai::AuthContext& context,
                          std::optional<ai::ApiKeyCredential> credential)
        -> boost::asio::awaitable<util::Expected<std::optional<ai::AuthResult>>> {
        std::optional<std::string> key = credential ? credential->key : std::nullopt;
        std::string source = credential ? "credential" : "ambient";
        if (!key) {
            CCH_TRY(ambient, co_await context.environment("API_KEY"));
            key = std::move(ambient);
        }
        if (!key) {
            co_return std::optional<ai::AuthResult>{};
        }
        resolved_keys.push_back(*key);
        co_return ai::AuthResult{
            .auth = ai::ModelAuth{.api_key = *key},
            .env = {},
            .source = source,
        };
    };

    auto models = make_models(credentials, auth_context);
    auto provider = std::make_shared<RecordingProvider>(
        "provider", ai::ProviderAuth{.api_key = std::move(api_key)});
    REQUIRE(models->set_provider(provider));

    ai::Model explicit_request = tests::make_model("model", "provider", "api");
    std::vector<ai::AssistantStreamEvent> explicit_events;
    auto explicit_result = run_awaitable(ai::consume(
        models->stream(
            explicit_request,
            {},
            ai::SimpleStreamOptions{.api_key = "explicit-key"}),
        [&explicit_events](const ai::AssistantStreamEvent& event) -> util::ExpectedVoid {
            explicit_events.push_back(event);
            return {};
        }));
    REQUIRE(explicit_result);
    REQUIRE(provider->seen_options.size() == 1);
    CHECK(provider->seen_options.back().auth.api_key == "explicit-key");

    ai::Model stored_request = tests::make_model("model", "provider", "api");
    REQUIRE(run_models(models, std::move(stored_request)).result);
    REQUIRE(provider->seen_options.size() == 2);
    CHECK(provider->seen_options.back().auth.api_key == "stored-key");

    credentials->records.clear();
    ai::Model ambient_request = tests::make_model("model", "provider", "api");
    REQUIRE(run_models(models, std::move(ambient_request)).result);
    REQUIRE(provider->seen_options.size() == 3);
    CHECK(provider->seen_options.back().auth.api_key == "ambient-key");
    CHECK((resolved_keys == std::vector<std::string>{"explicit-key", "stored-key", "ambient-key"}));
}

TEST_CASE("Models never falls back after a stored credential type mismatch", "[ai][models][auth][issue338]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    credentials->records.emplace("provider", ai::Credential{ai::OAuthCredential{}});
    auto auth_context = std::make_shared<FakeAuthContext>();
    int resolve_count = 0;
    ai::ApiKeyAuth api_key;
    api_key.name = "key";
    api_key.resolve = [&resolve_count](const ai::AuthContext&, std::optional<ai::ApiKeyCredential>)
        -> boost::asio::awaitable<util::Expected<std::optional<ai::AuthResult>>> {
        ++resolve_count;
        co_return ai::AuthResult{};
    };
    auto models = make_models(credentials, auth_context);
    REQUIRE(models->set_provider(std::make_shared<RecordingProvider>(
        "provider", ai::ProviderAuth{.api_key = std::move(api_key)})));

    ai::Model request = tests::make_model("model", "provider", "api");
    auto run = run_models(models, std::move(request));

    REQUIRE(run.result);
    const auto& terminal = require_terminal_error(run);
    REQUIRE(terminal.failure);
    CHECK(terminal.failure->code == util::ErrorCode::Auth);
    CHECK(resolve_count == 0);
}

TEST_CASE("Models refreshes OAuth under the store mutation and checkAuth never refreshes", "[ai][models][auth][issue338]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    credentials->records.emplace(
        "provider",
        ai::Credential{ai::OAuthCredential{
            .refresh = "refresh",
            .access = "old-access",
            .expires = now,
        }});
    auto auth_context = std::make_shared<FakeAuthContext>();
    int refresh_count = 0;
    ai::OAuthAuth oauth;
    oauth.name = "oauth";
    oauth.refresh = [&refresh_count](ai::OAuthCredential credential)
        -> boost::asio::awaitable<util::Expected<ai::OAuthCredential>> {
        ++refresh_count;
        credential.access = "new-access";
        credential.expires += 60 * 60 * 1000;
        co_return credential;
    };
    oauth.to_auth = [](const ai::OAuthCredential& credential)
        -> boost::asio::awaitable<util::Expected<ai::ModelAuth>> {
        co_return ai::ModelAuth{
            .headers = {{"Authorization", "Bearer " + credential.access}},
        };
    };

    auto models = make_models(credentials, auth_context);
    auto provider = std::make_shared<RecordingProvider>(
        "provider", ai::ProviderAuth{.oauth = std::move(oauth)});
    REQUIRE(models->set_provider(provider));

    auto checked = run_awaitable(models->check_auth("provider"));
    REQUIRE(checked);
    REQUIRE(*checked);
    CHECK((**checked).type == ai::AuthType::OAuth);
    CHECK(refresh_count == 0);
    CHECK(credentials->modify_count == 0);

    ai::Model request = tests::make_model("model", "provider", "api");
    auto run = run_models(models, std::move(request));
    REQUIRE(run.result);
    CHECK(refresh_count == 1);
    CHECK(credentials->modify_count == 1);
    REQUIRE(provider->seen_options.size() == 1);
    CHECK(provider->seen_options.front().auth.headers.at("Authorization") == "Bearer new-access");
    const auto& stored = std::get<ai::OAuthCredential>(credentials->records.at("provider"));
    CHECK(stored.access == "new-access");
}

TEST_CASE("Models preserves stored OAuth when refresh fails", "[ai][models][auth][issue338]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    ai::OAuthCredential original{
        .refresh = "refresh",
        .access = "old-access",
        .expires = 0,
    };
    credentials->records.emplace("provider", ai::Credential{original});
    auto auth_context = std::make_shared<FakeAuthContext>();
    ai::OAuthAuth oauth;
    oauth.name = "oauth";
    oauth.refresh = [](ai::OAuthCredential)
        -> boost::asio::awaitable<util::Expected<ai::OAuthCredential>> {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Network,
            "refresh rejected"));
    };
    oauth.to_auth = [](const ai::OAuthCredential&)
        -> boost::asio::awaitable<util::Expected<ai::ModelAuth>> {
        co_return ai::ModelAuth{};
    };
    auto models = make_models(credentials, auth_context);
    REQUIRE(models->set_provider(std::make_shared<RecordingProvider>(
        "provider", ai::ProviderAuth{.oauth = std::move(oauth)})));

    ai::Model request = tests::make_model("model", "provider", "api");
    auto run = run_models(models, std::move(request));

    REQUIRE(run.result);
    const auto& terminal = require_terminal_error(run);
    REQUIRE(terminal.failure);
    CHECK(terminal.failure->code == util::ErrorCode::OAuth);
    CHECK(std::get<ai::OAuthCredential>(credentials->records.at("provider")) == original);
}

TEST_CASE("Models merges Model headers after resolved auth headers case insensitively", "[ai][models][auth][issue338]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    auto auth_context = std::make_shared<FakeAuthContext>();
    ai::ApiKeyAuth api_key;
    api_key.name = "headers";
    api_key.resolve = [](const ai::AuthContext&, std::optional<ai::ApiKeyCredential>)
        -> boost::asio::awaitable<util::Expected<std::optional<ai::AuthResult>>> {
        co_return ai::AuthResult{
            .auth = ai::ModelAuth{
                .headers = {
                    {"X-Test", "auth"},
                    {"x-TEST", "duplicate auth"},
                    {"Authorization", "Bearer dummy"},
                },
            },
            .source = "headers",
        };
    };
    auto models = make_models(credentials, auth_context);
    auto provider = std::make_shared<RecordingProvider>(
        "provider", ai::ProviderAuth{.api_key = std::move(api_key)});
    REQUIRE(models->set_provider(provider));

    ai::Model request = tests::make_model("model", "provider", "api");
    request.headers = ai::ModelHeaders{{"x-test", "model"}};
    REQUIRE(run_models(models, std::move(request)).result);

    REQUIRE(provider->seen_options.size() == 1);
    const auto& headers = provider->seen_options.front().auth.headers;
    CHECK(headers.at("x-test") == "model");
    CHECK(headers.at("Authorization") == "Bearer dummy");
    CHECK(headers.size() == 2);
}

TEST_CASE("Models prepares the complete streamSimple request before Provider dispatch", "[ai][models][issue339]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    auto auth_context = std::make_shared<FakeAuthContext>();
    ai::ApiKeyAuth api_key;
    api_key.name = "prepared";
    api_key.resolve = [](const ai::AuthContext&, std::optional<ai::ApiKeyCredential>)
        -> boost::asio::awaitable<util::Expected<std::optional<ai::AuthResult>>> {
        co_return ai::AuthResult{
            .auth = ai::ModelAuth{
                .api_key = "dummy-key",
                .headers = {{"X-Auth", "auth"}, {"X-Delete", "remove"}},
            },
            .env = {{"A", "auth"}, {"PI_CACHE_RETENTION", "short"}},
            .source = "prepared",
        };
    };
    auto models = make_models(credentials, auth_context);
    auto provider = std::make_shared<RecordingProvider>(
        "deepseek", ai::ProviderAuth{.api_key = std::move(api_key)});
    REQUIRE(models->set_provider(provider));

    auto model = tests::make_model("reasoning", "deepseek", "openai-responses");
    model.reasoning = true;
    model.context_window = 10000;
    model.max_tokens = 9000;
    model.headers = ai::ModelHeaders{{"X-Model", "model"}};
    ai::AiContext context;
    context.system_prompt = std::string(4000, 'x');
    int transform_count = 0;
    ai::SimpleStreamOptions options;
    options.temperature = 0.25;
    options.max_tokens = 9000;
    options.headers = {
        {"x-auth", std::string{"request"}},
        {"x-delete", std::nullopt},
    };
    options.env = {{"A", "request"}, {"PI_CACHE_RETENTION", "long"}};
    options.transform_headers = [&transform_count](ai::RequestHeaders headers)
        -> util::Expected<ai::RequestHeaders> {
        ++transform_count;
        CHECK(headers.at("session_id") == "session-1");
        headers.insert_or_assign("X-Transformed", "yes");
        return headers;
    };
    options.reasoning = ai::ThinkingLevel::High;
    options.session_id = "session-1";
    options.timeout_ms = 3210;
    options.max_retries = 2;
    options.max_retry_delay_ms = 12345;

    std::vector<ai::AssistantStreamEvent> events;
    auto result = run_awaitable(ai::consume(
        models->stream(
            std::move(model),
            std::move(context),
            std::move(options)),
        [&events](const ai::AssistantStreamEvent& event) -> util::ExpectedVoid {
            events.push_back(event);
            return {};
        }));

    REQUIRE(result);
    CHECK(transform_count == 1);
    REQUIRE(provider->seen_options.size() == 1);
    const auto& prepared = provider->seen_options.front();
    CHECK(prepared.temperature == 0.25);
    CHECK(prepared.max_tokens == 4904);
    CHECK(prepared.reasoning == ai::ModelThinkingLevel::High);
    CHECK(prepared.session_id == "session-1");
    CHECK(prepared.cache_retention == ai::CacheRetention::Long);
    CHECK(prepared.timeout_ms == 3210);
    CHECK(prepared.max_retries == 2);
    CHECK(prepared.max_retry_delay_ms == 12345);
    CHECK(prepared.env.at("A") == "request");
    CHECK(prepared.auth.headers.at("x-auth") == "request");
    CHECK(prepared.auth.headers.at("X-Model") == "model");
    CHECK(prepared.auth.headers.at("X-Transformed") == "yes");
    CHECK_FALSE(prepared.auth.headers.contains("X-Delete"));
    CHECK(prepared.auth.headers.at("x-client-request-id") == "session-1");
}

TEST_CASE("Models prepares Codex session affinity headers", "[ai][models][issue339]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    auto auth_context = std::make_shared<FakeAuthContext>();
    ai::ApiKeyAuth api_key;
    api_key.name = "codex";
    api_key.resolve = [](const ai::AuthContext&, std::optional<ai::ApiKeyCredential>)
        -> boost::asio::awaitable<util::Expected<std::optional<ai::AuthResult>>> {
        co_return ai::AuthResult{
            .auth = ai::ModelAuth{.api_key = "dummy-codex"},
            .source = "codex",
        };
    };
    auto models = make_models(credentials, auth_context);
    auto provider = std::make_shared<RecordingProvider>(
        "openai-codex", ai::ProviderAuth{.api_key = std::move(api_key)});
    REQUIRE(models->set_provider(provider));
    auto model = tests::make_model(
        "gpt-5.5", "openai-codex", "openai-codex-responses");
    ai::SimpleStreamOptions options;
    options.session_id = std::string(65, 's');
    std::vector<ai::AssistantStreamEvent> events;

    auto result = run_awaitable(ai::consume(
        models->stream(
            std::move(model),
            ai::AiContext{},
            std::move(options)),
        [&events](const ai::AssistantStreamEvent& event) -> util::ExpectedVoid {
            events.push_back(event);
            return {};
        }));

    REQUIRE(result);
    REQUIRE(provider->seen_options.size() == 1);
    const auto& headers = provider->seen_options.front().auth.headers;
    CHECK(headers.at("session-id") == std::string(64, 's'));
    CHECK(headers.at("x-client-request-id") == std::string(64, 's'));
    CHECK(provider->seen_options.front().session_id == std::string(65, 's'));
}

TEST_CASE(
    "Models accepts Kimi header authentication and suppresses none-retention affinity",
    "[ai][models][auth][issue339]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    auto auth_context = std::make_shared<FakeAuthContext>();
    ai::ApiKeyAuth api_key;
    api_key.name = "header auth";
    api_key.resolve = [](const ai::AuthContext&, std::optional<ai::ApiKeyCredential>)
        -> boost::asio::awaitable<util::Expected<std::optional<ai::AuthResult>>> {
        co_return ai::AuthResult{
            .auth = ai::ModelAuth{
                .headers = {{"Authorization", "Bearer dummy-oauth"}},
            },
            .source = "OAuth",
        };
    };
    auto models = make_models(credentials, auth_context);
    auto provider = std::make_shared<RecordingProvider>(
        "kimi-coding", ai::ProviderAuth{.api_key = std::move(api_key)});
    REQUIRE(models->set_provider(provider));
    auto model = tests::make_model(
        "kimi-for-coding", "kimi-coding", "anthropic-messages");
    ai::SimpleStreamOptions options;
    options.session_id = "ignored-session";
    options.cache_retention = ai::CacheRetention::None;
    std::vector<ai::AssistantStreamEvent> events;

    auto result = run_awaitable(ai::consume(
        models->stream(
            std::move(model),
            ai::AiContext{},
            std::move(options)),
        [&events](const ai::AssistantStreamEvent& event) -> util::ExpectedVoid {
            events.push_back(event);
            return {};
        }));

    REQUIRE(result);
    REQUIRE(provider->seen_options.size() == 1);
    CHECK(provider->seen_options.front().auth.api_key == std::nullopt);
    CHECK(provider->seen_options.front().auth.headers.at("Authorization") ==
          "Bearer dummy-oauth");
    CHECK(provider->seen_options.front().session_id == std::nullopt);
}

TEST_CASE("Env-chain API key auth labels explicit credentials as stored credentials", "[ai][models][auth][issue338]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    auto auth_context = std::make_shared<FakeAuthContext>();
    auto models = make_models(credentials, auth_context);
    REQUIRE(models->set_provider(std::make_shared<RecordingProvider>(
        "provider",
        ai::providers::make_env_api_key_auth("API key", {"API_KEY"}))));

    auto resolved = run_awaitable(models->get_auth("provider", "explicit-key"));

    REQUIRE(resolved);
    REQUIRE(*resolved);
    CHECK((**resolved).source == "stored credential");
}

TEST_CASE("Models converts throwing callbacks into its single error channel", "[ai][models][issue338]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    auto auth_context = std::make_shared<FakeAuthContext>();

    ai::ApiKeyAuth throwing_auth;
    throwing_auth.name = "throwing auth";
    throwing_auth.resolve = [](
        const ai::AuthContext&,
        std::optional<ai::ApiKeyCredential>)
        -> boost::asio::awaitable<util::Expected<std::optional<ai::AuthResult>>> {
        throw std::runtime_error{"auth callback threw"};
        co_return std::optional<ai::AuthResult>{};
    };
    auto auth_models = make_models(credentials, auth_context);
    REQUIRE(auth_models->set_provider(std::make_shared<RecordingProvider>(
        "auth-provider",
        ai::ProviderAuth{.api_key = std::move(throwing_auth)})));
    ai::Model auth_request = tests::make_model("model", "auth-provider", "api");

    auto auth_run = run_models(auth_models, std::move(auth_request));

    REQUIRE(auth_run.result);
    const auto& auth_terminal = require_terminal_error(auth_run);
    REQUIRE(auth_terminal.failure);
    CHECK(auth_terminal.failure->code == util::ErrorCode::Auth);

    auto provider_models = make_models(credentials, auth_context);
    auto throwing_provider = std::make_shared<RecordingProvider>("provider");
    throwing_provider->throw_stream = true;
    REQUIRE(provider_models->set_provider(throwing_provider));
    ai::Model provider_request = tests::make_model("model", "provider", "api");

    auto provider_run = run_models(provider_models, std::move(provider_request));

    REQUIRE(provider_run.result);
    const auto& provider_terminal = require_terminal_error(provider_run);
    REQUIRE(provider_terminal.failure);
    CHECK(provider_terminal.failure->code == util::ErrorCode::Stream);

    auto sink_models = make_models(credentials, auth_context);
    REQUIRE(sink_models->set_provider(std::make_shared<RecordingProvider>("sink-provider")));
    ai::Model sink_request = tests::make_model("model", "sink-provider", "api");
    auto sink_result = run_awaitable(ai::consume(
        sink_models->stream(
            std::move(sink_request),
            {},
            {}),
        [](const ai::AssistantStreamEvent&) -> util::ExpectedVoid {
            throw std::runtime_error{"sink callback threw"};
        }));

    REQUIRE_FALSE(sink_result);
    CHECK(sink_result.error().code == util::ErrorCode::Unknown);
    CHECK(sink_result.error().message == "Assistant event sink failed");
}

TEST_CASE("Models categorizes throwing credential store and OAuth callbacks", "[ai][models][issue338]") {
    auto auth_context = std::make_shared<FakeAuthContext>();

    auto throwing_store = std::make_shared<MemoryCredentialStore>();
    throwing_store->throw_read = true;
    auto store_models = make_models(throwing_store, auth_context);
    REQUIRE(store_models->set_provider(std::make_shared<RecordingProvider>("store-provider")));
    ai::Model store_request = tests::make_model("model", "store-provider", "api");

    auto store_run = run_models(store_models, std::move(store_request));

    REQUIRE(store_run.result);
    const auto& store_terminal = require_terminal_error(store_run);
    REQUIRE(store_terminal.failure);
    CHECK(store_terminal.failure->code == util::ErrorCode::Auth);

    auto refresh_credentials = std::make_shared<MemoryCredentialStore>();
    refresh_credentials->records.emplace(
        "refresh-provider",
        ai::OAuthCredential{.expires = 0});
    ai::OAuthAuth refresh_auth;
    refresh_auth.name = "throwing refresh";
    refresh_auth.refresh = [](ai::OAuthCredential)
        -> boost::asio::awaitable<util::Expected<ai::OAuthCredential>> {
        throw std::runtime_error{"OAuth refresh callback threw"};
        co_return ai::OAuthCredential{};
    };
    refresh_auth.to_auth = [](const ai::OAuthCredential&)
        -> boost::asio::awaitable<util::Expected<ai::ModelAuth>> {
        co_return ai::ModelAuth{};
    };
    auto refresh_models = make_models(refresh_credentials, auth_context);
    REQUIRE(refresh_models->set_provider(std::make_shared<RecordingProvider>(
        "refresh-provider",
        ai::ProviderAuth{.oauth = std::move(refresh_auth)})));
    ai::Model refresh_request = tests::make_model("model", "refresh-provider", "api");

    auto refresh_run = run_models(refresh_models, std::move(refresh_request));

    REQUIRE(refresh_run.result);
    const auto& refresh_terminal = require_terminal_error(refresh_run);
    REQUIRE(refresh_terminal.failure);
    CHECK(refresh_terminal.failure->code == util::ErrorCode::OAuth);

    auto derivation_credentials = std::make_shared<MemoryCredentialStore>();
    derivation_credentials->records.emplace(
        "derivation-provider",
        ai::OAuthCredential{.expires = std::numeric_limits<std::int64_t>::max()});
    ai::OAuthAuth derivation_auth;
    derivation_auth.name = "throwing derivation";
    derivation_auth.to_auth = [](const ai::OAuthCredential&)
        -> boost::asio::awaitable<util::Expected<ai::ModelAuth>> {
        throw std::runtime_error{"OAuth derivation callback threw"};
        co_return ai::ModelAuth{};
    };
    auto derivation_models = make_models(derivation_credentials, auth_context);
    REQUIRE(derivation_models->set_provider(std::make_shared<RecordingProvider>(
        "derivation-provider",
        ai::ProviderAuth{.oauth = std::move(derivation_auth)})));
    ai::Model derivation_request = tests::make_model("model", "derivation-provider", "api");

    auto derivation_run = run_models(derivation_models, std::move(derivation_request));

    REQUIRE(derivation_run.result);
    const auto& derivation_terminal = require_terminal_error(derivation_run);
    REQUIRE(derivation_terminal.failure);
    CHECK(derivation_terminal.failure->code == util::ErrorCode::OAuth);
}

TEST_CASE("Models normalizes Provider stream failures and propagates sink failures", "[ai][models][issue338]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    auto auth_context = std::make_shared<FakeAuthContext>();
    auto models = make_models(credentials, auth_context);
    auto provider = std::make_shared<RecordingProvider>("provider");
    provider->stream_failure = util::make_error(util::ErrorCode::Stream, "serialization failed");
    REQUIRE(models->set_provider(provider));

    ai::Model request = tests::make_model("model", "provider", "api");
    auto normalized = run_models(models, request);
    REQUIRE(normalized.result);
    const auto& terminal = require_terminal_error(normalized);
    REQUIRE(terminal.failure);
    CHECK(terminal.failure->code == util::ErrorCode::Stream);
    CHECK(normalized.result->error_message == terminal.error.error_message);

    auto sink_failure = run_models(
        models,
        std::move(request),
        {},
        {},
        true,
        util::ErrorCode::Stream);
    REQUIRE_FALSE(sink_failure.result);
    CHECK(sink_failure.result.error().code == util::ErrorCode::Stream);
    CHECK(sink_failure.result.error().message == "consumer sink failed");
    CHECK(sink_failure.events.size() == 1);
}

TEST_CASE("Models cancellation is one aborted terminal value", "[ai][models][issue338]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    auto auth_context = std::make_shared<FakeAuthContext>();
    auto models = make_models(credentials, auth_context);
    auto provider = std::make_shared<RecordingProvider>("provider");
    REQUIRE(models->set_provider(provider));
    std::stop_source stop;
    stop.request_stop();

    ai::Model request = tests::make_model("model", "provider", "api");
    auto run = run_models(
        models,
        std::move(request),
        {},
        ai::SimpleStreamOptions{.stop_token = stop.get_token()});

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Aborted);
    CHECK(run.result->error_message == "Request was aborted");
    const auto& terminal = require_terminal_error(run);
    CHECK(terminal.reason == ai::AssistantStopReason::Aborted);
    REQUIRE(terminal.failure);
    CHECK(terminal.failure->code == util::ErrorCode::Cancelled);
    CHECK(provider->seen_models.size() == 1);
}

TEST_CASE("Models checkAuth falls back to API key resolution when no check hook exists", "[ai][models][auth][issue338]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    auto auth_context = std::make_shared<FakeAuthContext>();
    ai::ApiKeyAuth api_key;
    api_key.name = "fallback";
    api_key.resolve = [](const ai::AuthContext&, std::optional<ai::ApiKeyCredential>)
        -> boost::asio::awaitable<util::Expected<std::optional<ai::AuthResult>>> {
        co_return ai::AuthResult{.source = "resolved fallback"};
    };
    auto models = make_models(credentials, auth_context);
    REQUIRE(models->set_provider(std::make_shared<RecordingProvider>(
        "provider", ai::ProviderAuth{.api_key = std::move(api_key)})));

    auto checked = run_awaitable(models->check_auth("provider"));
    REQUIRE(checked);
    REQUIRE(*checked);
    CHECK((**checked).source == "resolved fallback");
    CHECK((**checked).type == ai::AuthType::ApiKey);
}

TEST_CASE("Models sanitizes Provider-emitted terminal errors", "[ai][models][issue338]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    auto auth_context = std::make_shared<FakeAuthContext>();
    auto models = make_models(credentials, auth_context);
    REQUIRE(models->set_provider(std::make_shared<UnsafeTerminalProvider>()));

    ai::Model request = tests::make_model("model", "unsafe-terminal", "api");
    auto run = run_models(models, std::move(request));

    REQUIRE(run.result);
    const auto& terminal = require_terminal_error(run);
    REQUIRE(run.result->error_message);
    CHECK(run.result->error_message->size() <= 1024);
    CHECK(run.result->error_message->find("sk-abcdefghijklmnopqrstuvwxyz1234567890") ==
          std::string::npos);
    REQUIRE(terminal.failure);
    CHECK(terminal.failure->message.size() <= 1024);
    CHECK(terminal.failure->message.find("sk-abcdefghijklmnopqrstuvwxyz1234567890") ==
          std::string::npos);
    CHECK(terminal.error.error_message == run.result->error_message);
}

TEST_CASE("Models suppresses duplicate Provider terminals and returns the first terminal value", "[ai][models][issue338]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    auto auth_context = std::make_shared<FakeAuthContext>();
    auto models = make_models(credentials, auth_context);
    REQUIRE(models->set_provider(std::make_shared<DuplicateTerminalProvider>()));

    ai::Model request = tests::make_model("model", "duplicate", "api");
    auto run = run_models(models, std::move(request));

    REQUIRE(run.result);
    CHECK(run.result->error_message == "first failure");
    REQUIRE(run.events.size() == 1);
    CHECK(std::holds_alternative<ai::AssistantErrorEvent>(run.events.front()));
}

TEST_CASE("Models live lookup and logout use owned Provider and CredentialStore state", "[ai][models][auth][issue338]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    credentials->records.emplace("provider", ai::Credential{ai::ApiKeyCredential{}});
    auto auth_context = std::make_shared<FakeAuthContext>();
    auto models = make_models(credentials, auth_context);
    auto provider = std::make_shared<RecordingProvider>("provider");
    provider->catalog.push_back(tests::make_model("catalog-model", "provider", "api"));
    REQUIRE(models->set_provider(provider));

    REQUIRE(models->model("provider", "catalog-model"));
    CHECK(models->model("provider", "missing") == std::nullopt);
    auto unknown_auth = run_awaitable(models->get_auth("unknown"));
    REQUIRE(unknown_auth);
    CHECK_FALSE(*unknown_auth);
    REQUIRE(run_awaitable(models->logout("provider")));
    CHECK(credentials->remove_count == 1);
    CHECK_FALSE(credentials->records.contains("provider"));
}

TEST_CASE("Models login persists the provider OAuth credential via CredentialStore modify", "[ai][models][auth][issue343]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    auto auth_context = std::make_shared<FakeAuthContext>();
    auto models = make_models(credentials, auth_context);
    auto provider = std::make_shared<RecordingProvider>(
        "login-provider",
        oauth_login_auth(
            [](ai::AuthInteraction) -> boost::asio::awaitable<util::Expected<ai::OAuthCredential>> {
                co_return ai::OAuthCredential{
                    .refresh = "dummy-refresh",
                    .access = "dummy-access",
                    .expires = 123,
                    .account_id = "account-xyz",
                };
            }));
    REQUIRE(models->set_provider(provider));

    auto result = run_awaitable(models->login(
        "login-provider", ai::AuthType::OAuth, empty_interaction()));

    REQUIRE(result);
    const auto* oauth = std::get_if<ai::OAuthCredential>(&*result);
    REQUIRE(oauth != nullptr);
    CHECK(oauth->account_id == std::string{"account-xyz"});
    CHECK(credentials->modify_count == 1);
    REQUIRE(credentials->records.contains("login-provider"));
    const auto* stored = std::get_if<ai::OAuthCredential>(&credentials->records.at("login-provider"));
    REQUIRE(stored != nullptr);
    CHECK(stored->access == "dummy-access");
    CHECK(stored->refresh == "dummy-refresh");
}

TEST_CASE("Models login flow failure propagates unwrapped to the host", "[ai][models][auth][issue343]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    auto auth_context = std::make_shared<FakeAuthContext>();
    auto models = make_models(credentials, auth_context);
    auto provider = std::make_shared<RecordingProvider>(
        "login-provider",
        oauth_login_auth(
            [](ai::AuthInteraction) -> boost::asio::awaitable<util::Expected<ai::OAuthCredential>> {
                co_return std::unexpected(util::make_error(
                    util::ErrorCode::Network,
                    "provider flow failed",
                    "detail"));
            }));
    REQUIRE(models->set_provider(provider));

    auto result = run_awaitable(models->login(
        "login-provider", ai::AuthType::OAuth, empty_interaction()));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == util::ErrorCode::Network);
    CHECK(result.error().message == "provider flow failed");
    CHECK(result.error().detail == "detail");
    CHECK(credentials->modify_count == 0);
    CHECK(credentials->records.empty());
}

TEST_CASE("Models login wraps CredentialStore modify failures as the auth category", "[ai][models][auth][issue343]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    credentials->fail_modify = true;
    auto auth_context = std::make_shared<FakeAuthContext>();
    auto models = make_models(credentials, auth_context);
    auto provider = std::make_shared<RecordingProvider>(
        "login-provider",
        oauth_login_auth(
            [](ai::AuthInteraction) -> boost::asio::awaitable<util::Expected<ai::OAuthCredential>> {
                co_return ai::OAuthCredential{
                    .refresh = "r", .access = "a", .expires = 1, .account_id = "acct"};
            }));
    REQUIRE(models->set_provider(provider));

    auto result = run_awaitable(models->login(
        "login-provider", ai::AuthType::OAuth, empty_interaction()));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == util::ErrorCode::Auth);
    CHECK(result.error().message == "Credential store modify failed for login-provider");
}

TEST_CASE("Models login rejects unknown providers as a provider error", "[ai][models][auth][issue343]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    auto auth_context = std::make_shared<FakeAuthContext>();
    auto models = make_models(credentials, auth_context);

    auto result = run_awaitable(models->login(
        "missing", ai::AuthType::OAuth, empty_interaction()));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == util::ErrorCode::Provider);
    CHECK(result.error().message == "Unknown provider: missing");
}

TEST_CASE("Models login rejects a provider without OAuth login support", "[ai][models][auth][issue343]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    auto auth_context = std::make_shared<FakeAuthContext>();
    auto models = make_models(credentials, auth_context);
    auto provider = std::make_shared<RecordingProvider>("key-only", keyless_auth());
    REQUIRE(models->set_provider(provider));

    auto result = run_awaitable(models->login(
        "key-only", ai::AuthType::OAuth, empty_interaction()));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == util::ErrorCode::Auth);
    CHECK(result.error().message == "key-only does not support oauth login");
    CHECK(credentials->modify_count == 0);
}

TEST_CASE("Models login persists an api-key credential through modify", "[ai][models][auth][issue343]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    auto auth_context = std::make_shared<FakeAuthContext>();
    auto models = make_models(credentials, auth_context);
    auto provider = std::make_shared<RecordingProvider>(
        "api-provider",
        api_key_login_auth(
            [](ai::AuthInteraction) -> boost::asio::awaitable<util::Expected<ai::ApiKeyCredential>> {
                ai::ApiKeyCredential credential;
                credential.key = "dummy-api-key";
                co_return credential;
            }));
    REQUIRE(models->set_provider(provider));

    auto result = run_awaitable(models->login(
        "api-provider", ai::AuthType::ApiKey, empty_interaction()));

    REQUIRE(result);
    const auto* stored = std::get_if<ai::ApiKeyCredential>(&*result);
    REQUIRE(stored != nullptr);
    CHECK(stored->key == std::string{"dummy-api-key"});
    CHECK(credentials->modify_count == 1);
    REQUIRE(credentials->records.contains("api-provider"));
    const auto* persisted = std::get_if<ai::ApiKeyCredential>(&credentials->records.at("api-provider"));
    REQUIRE(persisted != nullptr);
    CHECK(persisted->key == std::string{"dummy-api-key"});
}

TEST_CASE("Models login rejects a provider without api-key login support", "[ai][models][auth][issue343]") {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    auto auth_context = std::make_shared<FakeAuthContext>();
    auto models = make_models(credentials, auth_context);
    auto provider = std::make_shared<RecordingProvider>(
        "oauth-only",
        oauth_login_auth(
            [](ai::AuthInteraction) -> boost::asio::awaitable<util::Expected<ai::OAuthCredential>> {
                co_return ai::OAuthCredential{};
            }));
    REQUIRE(models->set_provider(provider));

    auto result = run_awaitable(models->login(
        "oauth-only", ai::AuthType::ApiKey, empty_interaction()));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == util::ErrorCode::Auth);
    CHECK(result.error().message == "oauth-only does not support api_key login");
}
