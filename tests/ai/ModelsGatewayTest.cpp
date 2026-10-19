#include <cch/ai/Models.hpp>

#include "ai/Headers.hpp"
#include "ai/MagpieCatalog.hpp"
#include "ai/ModelsTestAccess.hpp"
#include "support/AiScenarioKit.hpp"
#include "support/ModelFixture.hpp"
#include "support/StreamAdapterFixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_future.hpp>

#include <algorithm>
#include <memory>
#include <stop_token>
#include <string>

using namespace cch;

namespace {

[[nodiscard]] std::shared_ptr<ai::Models> gateway(
        std::shared_ptr<support::StreamTransport> transport, ai::ModelsOptions options = {}) {
    auto models = std::make_shared<ai::Models>(std::move(options));
    ai::detail::ModelsTestAccess::set_transport(*models, std::move(transport));
    return models;
}

constexpr std::string_view kCatalog = R"({"data":[{
    "id":"group/auto","display_name":"Auto","context_window":1000000,
    "max_output_tokens":131072,"reasoning":true,
    "supported_reasoning_levels":[{"effort":"low"},{"effort":"high"},{"effort":"max"}],
    "modalities":{"input":["text","image"]}
}]})";

} // namespace

TEST_CASE("Models uses one gateway target and key for discovery inspection and inference", "[ai][models][gateway]") {
    auto wire = std::make_shared<tests::ScriptedTransport>();
    wire->attempts = {
            {.chunks = {std::string{kCatalog}}},
            {.chunks = {R"({"name":"magpie","version":"test-version"})"}},
            {.chunks = {R"({"data":[{"id":"inspection-only"}]})"}},
            {.chunks = {"data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}\n\n"}},
    };
    auto models = gateway(wire, {.gateway_url = "http://gateway.invalid:4321///"});
    auto discovery = models->refresh();
    REQUIRE(models->configure_startup_key("invocation-key"));
    REQUIRE(tests::run_async_result(std::move(discovery)));
    auto model = models->model("magpie", "group/auto");
    REQUIRE(model);
    CHECK(model->name == "Auto");
    CHECK(model->context_window == 1000000);
    CHECK(model->max_tokens == 131072);
    CHECK(model->reasoning);
    CHECK(model->input == std::vector<ai::ModelInput>{ai::ModelInput::Text, ai::ModelInput::Image});
    REQUIRE(model->thinking_level_map);
    CHECK(model->thinking_level_map->at(ai::ModelThinkingLevel::High) == "high");
    CHECK(model->thinking_level_map->at(ai::ModelThinkingLevel::Max) == "max");

    auto status = tests::run_async_result(models->inspect_gateway());
    REQUIRE(status);
    CHECK(status->base_url == "http://gateway.invalid:4321");
    CHECK(status->name == "magpie");
    CHECK(status->version == "test-version");
    REQUIRE(status->models.size() == 1);
    CHECK(status->models.front().id == "inspection-only");
    CHECK(models->model("magpie", "group/auto"));
    CHECK_FALSE(models->model("magpie", "inspection-only"));

    // Source identity cannot select a vendor endpoint: Model has no address.
    model->provider = "deepseek";
    auto result = tests::run_models(*models, *model, {}, {});
    REQUIRE(result.result);
    CHECK(ai::text_from_assistant_content(result.result->content) == "ok");
    REQUIRE(wire->requests.size() == 4);
    CHECK(wire->requests[0].url == "http://gateway.invalid:4321/v1/models");
    CHECK(wire->requests[1].url == "http://gateway.invalid:4321/api/hello");
    CHECK(wire->requests[2].url == "http://gateway.invalid:4321/v1/models");
    CHECK(wire->requests[3].url == "http://gateway.invalid:4321/v1/chat/completions");
    for (const auto& request : wire->requests) {
        CHECK(request.headers.at("Authorization") == "Bearer invocation-key");
    }
    CHECK_FALSE(models->discovery_error());
}

TEST_CASE("Models discovery maps minimal model metadata without a static fallback", "[ai][models][gateway]") {
    auto wire = std::make_shared<tests::ScriptedTransport>();
    wire->attempts = {{.chunks = {R"({"data":[null,{}, {"id":"plain"}]})"}}};
    auto models = gateway(wire);
    REQUIRE(tests::run_async_result(models->refresh()));
    REQUIRE(models->models().size() == 1);
    const auto model = models->models().front();
    CHECK(model.name == "plain");
    CHECK(model.api == "openai-completions");
    CHECK(model.context_window == 128000);
    CHECK(model.max_tokens == 16384);
    CHECK_FALSE(model.reasoning);
    CHECK(model.input == std::vector<ai::ModelInput>{ai::ModelInput::Text});
}

TEST_CASE("Models refresh clears failed catalogs and recovers while cancellation preserves state",
        "[ai][models][gateway][cancellation]") {
    auto wire = std::make_shared<tests::ScriptedTransport>();
    wire->attempts = {
            {.chunks = {R"({"data":[{"id":"old"}]})"}},
            {.failure = support::make_error(support::ErrorCode::Network, "offline")},
            {.chunks = {R"({"data":[{"id":"new"}]})"}},
    };
    auto models = gateway(wire);
    REQUIRE(tests::run_async_result(models->refresh()));
    std::stop_source stopped;
    stopped.request_stop();
    auto cancelled = tests::run_async_result(models->refresh(stopped.get_token()));
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == support::ErrorCode::Cancelled);
    CHECK(models->model("magpie", "old"));
    CHECK(wire->requests.size() == 1);
    auto failure = tests::run_async_result(models->refresh());
    REQUIRE_FALSE(failure);
    CHECK(models->models().empty());
    REQUIRE(models->discovery_error());
    const auto saved_error = models->discovery_error()->message;
    CHECK_FALSE(tests::run_async_result(models->refresh(stopped.get_token())));
    CHECK(models->discovery_error()->message == saved_error);
    REQUIRE(tests::run_async_result(models->refresh()));
    CHECK(models->model("magpie", "new"));
    CHECK_FALSE(models->discovery_error());
}

TEST_CASE("Models fixes its key after discovery inspection and failed requests", "[ai][models][gateway]") {
    for (const bool inspect : {false, true}) {
        auto wire = std::make_shared<tests::ScriptedTransport>();
        wire->attempts = {{.failure = support::make_error(support::ErrorCode::Network, "offline")}};
        wire->repeat_last_attempt = true;
        auto models = gateway(wire);
        if (inspect) {
            auto status = tests::run_async_result(models->inspect_gateway());
            REQUIRE(status);
            CHECK(status->connection_error);
            CHECK(status->catalog_error);
        } else {
            CHECK_FALSE(tests::run_async_result(models->refresh()));
        }
        CHECK(models->configure_startup_key("magpie"));
        CHECK_FALSE(models->configure_startup_key("different-key"));
        CHECK_FALSE(tests::run_async_result(models->refresh()));
        for (const auto& request : wire->requests) {
            CHECK(request.headers.at("Authorization") == "Bearer magpie");
        }
    }
}

TEST_CASE("Models cancellation before I/O leaves startup key configurable", "[ai][models][gateway][cancellation]") {
    auto wire = std::make_shared<tests::ScriptedTransport>();
    auto models = gateway(wire);
    std::stop_source stopped;
    stopped.request_stop();
    auto cancelled_refresh = tests::run_async_result(models->refresh(stopped.get_token()));
    REQUIRE_FALSE(cancelled_refresh);
    CHECK(cancelled_refresh.error().message == "model catalog refresh cancelled");
    auto cancelled_inspect = tests::run_async_result(models->inspect_gateway(stopped.get_token()));
    REQUIRE_FALSE(cancelled_inspect);
    CHECK(cancelled_inspect.error().message == "gateway status inspection cancelled");
    CHECK(wire->requests.empty());
    auto model = tests::make_model("cancelled", "magpie", "openai-completions");
    auto run = tests::run_models(*models, model, {}, {.stop_token = stopped.get_token()});
    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Aborted);
    CHECK(wire->requests.empty());
    CHECK(models->configure_startup_key("new-startup-key"));
    model.api = "unsupported-protocol";
    auto invalid = tests::run_models(*models, model, {}, {});
    REQUIRE(invalid.result);
    CHECK(invalid.result->stop_reason == ai::AssistantStopReason::Error);
    CHECK(wire->requests.empty());
    CHECK(models->configure_startup_key("still-configurable"));
}

TEST_CASE("Models inspection reports partial facts without changing catalog or discovery error",
        "[ai][models][gateway]") {
    auto wire = std::make_shared<tests::ScriptedTransport>();
    wire->attempts = {
            {.chunks = {R"({"data":[{"id":"retained"}]})"}},
            {.chunks = {R"({"name":"magpie","version":"v1"})"}},
            {.head = {.status_code = 503, .headers = {}}, .chunks = {"offline"}},
            {.failure = support::make_error(support::ErrorCode::Network, "hello unavailable")},
            {.chunks = {R"({"data":[{"id":"observed"}]})"}},
    };
    auto models = gateway(wire);
    REQUIRE(tests::run_async_result(models->refresh()));
    auto first = tests::run_async_result(models->inspect_gateway());
    REQUIRE(first);
    CHECK(first->name == "magpie");
    CHECK_FALSE(first->connection_error);
    CHECK(first->catalog_error);
    auto second = tests::run_async_result(models->inspect_gateway());
    REQUIRE(second);
    CHECK(second->connection_error);
    CHECK_FALSE(second->catalog_error);
    REQUIRE(second->models.size() == 1);
    CHECK(second->models.front().id == "observed");
    CHECK(models->model("magpie", "retained"));
    CHECK_FALSE(models->discovery_error());
    auto failed = tests::run_async_result(models->refresh());
    REQUIRE_FALSE(failed);
    const auto discovery_error = models->discovery_error()->message;
    auto observed = tests::run_async_result(models->inspect_gateway());
    REQUIRE(observed);
    CHECK(models->models().empty());
    CHECK(models->discovery_error()->message == discovery_error);
}

TEST_CASE("Models discovery rejects malformed catalogs with bounded redacted diagnostics",
        "[ai][models][gateway][redaction]") {
    for (const auto& body : {std::string{"bad json"}, std::string{"[]"}, std::string{"{}"}}) {
        auto wire = std::make_shared<tests::ScriptedTransport>();
        wire->attempts = {{.chunks = {body}}};
        auto models = gateway(wire);
        REQUIRE(ai::detail::ModelsTestAccess::replace_models(*models, {tests::make_model("previous")}));
        CHECK_FALSE(tests::run_async_result(models->refresh()));
        CHECK(models->models().empty());
        CHECK(models->discovery_error());
    }
    auto wire = std::make_shared<tests::ScriptedTransport>();
    wire->attempts = {{.head = {.status_code = 401, .headers = {}},
            .chunks = {"Authorization: Bearer sk-secret-for-gateway " + std::string(5000, 'x')}}};
    auto models = gateway(wire, {.api_key = "sk-secret-for-gateway"});
    auto result = tests::run_async_result(models->refresh());
    REQUIRE_FALSE(result);
    CHECK(result.error().message.size() <= 1024);
    CHECK(result.error().message.find("sk-secret-for-gateway") == std::string::npos);
    CHECK(result.error().message.find("[REDACTED]") != std::string::npos);
    auto echoed = std::make_shared<tests::ScriptedTransport>();
    echoed->attempts = {{.failure = support::make_error(support::ErrorCode::Network,
                                 "echo arbitrary-request-secret",
                                 "arbitrary-request-secret",
                                 "arbitrary-request-secret")}};
    auto private_key = gateway(echoed, {.api_key = "arbitrary-request-secret"});
    auto failed = tests::run_async_result(private_key->refresh());
    REQUIRE_FALSE(failed);
    CHECK(failed.error().message.find("arbitrary-request-secret") == std::string::npos);
    CHECK(failed.error().detail.find("arbitrary-request-secret") == std::string::npos);
    if (failed.error().context) CHECK(failed.error().context->find("arbitrary-request-secret") == std::string::npos);
}

TEST_CASE("Models pending discovery retains its lifetime and cancellation preserves the old catalog",
        "[ai][models][gateway][cancellation]") {
    auto wire = std::make_shared<tests::StopAwareBlockingTransport>();
    auto models = gateway(wire);
    REQUIRE(tests::run_async_result(models->refresh()));
    REQUIRE(ai::detail::ModelsTestAccess::replace_models(*models, {tests::make_model("old")}));
    std::stop_source stopped;
    auto pending = models->refresh(stopped.get_token());
    boost::asio::io_context io;
    auto future =
            boost::asio::co_spawn(io, support::detail::await_async_result(std::move(pending)), boost::asio::use_future);
    while (wire->request_count.load() < 2)
        REQUIRE(io.poll_one() != 0);
    stopped.request_stop();
    io.restart();
    io.run();
    auto result = future.get();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == support::ErrorCode::Cancelled);
    CHECK(models->model("fake", "old"));
    CHECK_FALSE(models->discovery_error());
    CHECK(wire->cancelled_requests.load() == 1);

    auto scripted = std::make_shared<tests::ScriptedTransport>();
    scripted->attempts = {{.chunks = {R"({"data":[{"id":"survived"}]})"}}};
    auto lifetime = gateway(scripted);
    auto discovery = lifetime->refresh();
    lifetime.reset();
    REQUIRE(tests::run_async_result(std::move(discovery)));
    CHECK(scripted->requests.size() == 1);
}

TEST_CASE("Models stream diagnostics scrub arbitrary gateway secrets and default magpie is not redacted",
        "[ai][models][stream][redaction]") {
    auto wire = std::make_shared<tests::ScriptedTransport>();
    wire->attempts = {{.failure = support::make_error(support::ErrorCode::Network,
                               "gateway secret-stream-token failed",
                               "secret-stream-token detail",
                               "secret-stream-token context")}};
    auto models = gateway(wire, {.api_key = "secret-stream-token"});
    auto model = tests::make_model("stream-model", "magpie", "openai-completions");
    auto run = tests::run_models(*models, model, {}, {});
    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.result->error_message);
    CHECK(run.result->error_message->find("secret-stream-token") == std::string::npos);
    CHECK(run.result->error_message->find("[REDACTED]") != std::string::npos);
    REQUIRE(!run.events.empty());
    const auto* err_event = std::get_if<ai::AssistantErrorEvent>(&run.events.back());
    REQUIRE(err_event != nullptr);
    if (err_event->failure) {
        CHECK(err_event->failure->message.find("secret-stream-token") == std::string::npos);
        CHECK(err_event->failure->detail.find("secret-stream-token") == std::string::npos);
    }

    // Default "magpie" placeholder is not redacted.
    auto magpie_wire = std::make_shared<tests::ScriptedTransport>();
    magpie_wire->attempts = {
            {.failure = support::make_error(support::ErrorCode::Network, "Cannot connect to magpie gateway")}};
    auto magpie_models = gateway(magpie_wire, {.api_key = "magpie"});
    auto magpie_run = tests::run_models(*magpie_models, model, {}, {});
    REQUIRE(magpie_run.result);
    REQUIRE(magpie_run.result->error_message);
    CHECK(magpie_run.result->error_message->find("magpie") != std::string::npos);
}

TEST_CASE("Models stream authoritatively sets Authorization and rejects caller header overrides",
        "[ai][models][stream][auth]") {
    auto wire = std::make_shared<tests::ScriptedTransport>();
    wire->attempts = {
            {.chunks = {"data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}\n\n"}}};
    auto models = gateway(wire, {.api_key = "authoritative-gateway-key"});
    auto model = tests::make_model("override-attempt", "magpie", "openai-completions");
    model.headers = ai::HeaderValues{{"Authorization", "Bearer hijacked-key"}};

    ai::SimpleStreamOptions options;
    options.headers = ai::RequestHeaders{{"Authorization", std::nullopt}};

    auto run = tests::run_models(*models, model, {}, std::move(options));
    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    REQUIRE(wire->requests.size() == 1);
    CHECK(wire->requests.front().headers.at("Authorization") == "Bearer authoritative-gateway-key");

    // A caller value cannot reach the wire either: the gateway key is the
    // only Authorization source (ADR 0072).
    auto value_wire = std::make_shared<tests::ScriptedTransport>();
    value_wire->attempts = {
            {.chunks = {"data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}\n\n"}}};
    auto value_models = gateway(value_wire, {.api_key = "authoritative-gateway-key"});

    ai::SimpleStreamOptions value_options;
    value_options.headers = ai::RequestHeaders{{"Authorization", "Bearer option-hijacked"}};

    auto value_run = tests::run_models(*value_models,
            tests::make_model("override-attempt", "magpie", "openai-completions"),
            {},
            std::move(value_options));
    REQUIRE(value_run.result);
    CHECK(value_run.result->stop_reason == ai::AssistantStopReason::Stop);
    CHECK(value_wire->requests.front().headers.at("Authorization") == "Bearer authoritative-gateway-key");
}

TEST_CASE("Models options defaults empty and slash-only gateway URLs", "[ai][models][gateway]") {
    for (const auto& empty_or_slashes : {"", "/", "////"}) {
        auto wire = std::make_shared<tests::ScriptedTransport>();
        wire->attempts = {{.chunks = {R"({"name":"magpie","version":"1"})"}}, {.chunks = {R"({"data":[]})"}}};
        auto models = gateway(wire, {.gateway_url = empty_or_slashes});
        auto status = tests::run_async_result(models->inspect_gateway());
        REQUIRE(status);
        CHECK(status->base_url == "http://127.0.0.1:3425");
        REQUIRE(wire->requests.size() >= 1);
        CHECK(wire->requests.front().url.starts_with("http://127.0.0.1:3425"));
    }
}

TEST_CASE("MagpieCatalog pure value parsers parse hello and model payloads directly", "[ai][models][catalog]") {
    const auto hello = ai::parse_magpie_hello_response(R"({"name":"test-gw","version":"9.9"})");
    REQUIRE(hello);
    CHECK(hello->name == "test-gw");
    CHECK(hello->version == "9.9");

    const auto models = ai::parse_magpie_models_response(R"({"data":[{"id":"direct-model"}]})");
    REQUIRE(models);
    REQUIRE(models->size() == 1);
    CHECK(models->front().id == "direct-model");
    CHECK(models->front().provider == "magpie");

    CHECK_FALSE(ai::parse_magpie_hello_response("invalid json"));
    CHECK_FALSE(ai::parse_magpie_models_response(R"({"not_data": []})"));
}

TEST_CASE("MagpieCatalog maps a gateway none level onto the Off thinking level", "[ai][models][catalog][spec]") {
    const auto models = ai::parse_magpie_models_response(R"({"data":[{
        "id":"group/auto","display_name":"Auto","reasoning":true,
        "supported_reasoning_levels":[{"effort":"none"},{"effort":"low"},{"effort":"high"}]
    }]})");
    REQUIRE(models);
    REQUIRE(models->size() == 1);
    const auto& model = models->front();
    REQUIRE(model.thinking_level_map);
    CHECK(model.thinking_level_map->at(ai::ModelThinkingLevel::Off) == "none");
    CHECK(model.thinking_level_map->at(ai::ModelThinkingLevel::Low) == "low");
    const auto supported = ai::get_supported_thinking_levels(model);
    CHECK(std::ranges::find(supported, ai::ModelThinkingLevel::Off) != supported.end());

    // Without a `none` record the gateway states the model cannot stop
    // thinking, so the Off level stays unselectable.
    const auto without_none = ai::parse_magpie_models_response(R"({"data":[{
        "id":"group/reasoned","display_name":"Reasoned","reasoning":true,
        "supported_reasoning_levels":[{"effort":"low"},{"effort":"high"}]
    }]})");
    REQUIRE(without_none);
    REQUIRE(without_none->front().thinking_level_map);
    CHECK_FALSE(without_none->front().thinking_level_map->at(ai::ModelThinkingLevel::Off).has_value());
    const auto without_supported = ai::get_supported_thinking_levels(without_none->front());
    CHECK(std::ranges::find(without_supported, ai::ModelThinkingLevel::Off) == without_supported.end());
}
