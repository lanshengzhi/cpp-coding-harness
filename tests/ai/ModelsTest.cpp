#include <cch/ai/Models.hpp>

#include "ai/ModelsTestAccess.hpp"
#include "support/AiScenarioKit.hpp"
#include "support/ModelFixture.hpp"
#include "support/StreamAdapterFixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

std::shared_ptr<tests::ScriptedTransport> successful_transport() {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .chunks = {"data: {\"choices\":[{\"delta\":{\"content\":\"hello\"},\"finish_reason\":\"stop\"}]}\n\n"
                       "data: [DONE]\n\n"},
    });
    return transport;
}

ai::Model gateway_model(std::string id = "upstream/model") {
    auto model = tests::make_model(std::move(id), "magpie", "openai-completions");
    return model;
}

} // namespace

TEST_CASE("Models resolves gateway key precedence through the emitted request", "[ai][models][gateway]") {
    struct Case {
        ai::ModelsOptions options;
        std::string expected;
    };
    const std::vector<Case> cases{
            {.options = {}, .expected = "magpie"},
            {.options = {.environment_api_key = "environment-key"}, .expected = "environment-key"},
            {.options = {.api_key = "explicit-key", .environment_api_key = "environment-key"},
                    .expected = "explicit-key"},
            {.options = {.environment_api_key = ""}, .expected = "magpie"},
    };
    for (const auto& test : cases) {
        auto models = std::make_shared<ai::Models>(test.options);
        auto transport = successful_transport();
        ai::detail::ModelsTestAccess::set_transport(*models, transport);
        auto result = tests::run_models(*models, gateway_model(), {}, {});
        REQUIRE(result.result);
        CHECK(result.result->stop_reason == ai::AssistantStopReason::Stop);
        REQUIRE(transport->requests.size() == 1);
        CHECK(transport->requests.front().headers.at("Authorization") == "Bearer " + test.expected);
        CHECK(transport->requests.front().url == "http://127.0.0.1:3425/v1/chat/completions");
        CHECK_FALSE(result.result->error_message);
    }
}

TEST_CASE("Models binds startup keys before consumption and rejects later mutation", "[ai][models][gateway]") {
    auto models = std::make_shared<ai::Models>(ai::ModelsOptions{.environment_api_key = "environment-key"});
    auto transport = successful_transport();
    ai::detail::ModelsTestAccess::set_transport(*models, transport);
    auto stream = models->stream(gateway_model(), {}, {});
    REQUIRE(models->configure_startup_key("startup-key"));
    auto result = tests::run_async_result(std::move(stream).run({}));
    REQUIRE(result);
    REQUIRE(transport->requests.size() == 1);
    CHECK(transport->requests.front().headers.at("Authorization") == "Bearer startup-key");
    CHECK(models->configure_startup_key("startup-key"));
    CHECK_FALSE(models->configure_startup_key("different-key"));
    CHECK_FALSE(models->configure_startup_key(""));
}

TEST_CASE("Models replaces passive catalogs atomically without provider installation", "[ai][models][catalog]") {
    auto models = std::make_shared<ai::Models>();
    REQUIRE(ai::detail::ModelsTestAccess::replace_models(*models, {gateway_model("old")}));
    auto invalid = gateway_model("invalid");
    invalid.api.clear();
    CHECK_FALSE(ai::detail::ModelsTestAccess::replace_models(*models, {gateway_model("new"), invalid}));
    CHECK(models->model("magpie", "old"));
    CHECK_FALSE(models->model("magpie", "new"));
    REQUIRE(ai::detail::ModelsTestAccess::replace_models(*models, {gateway_model("new")}));
    CHECK_FALSE(models->model("magpie", "old"));
    CHECK(models->model("magpie", "new"));
    CHECK(models->models("other").empty());
    REQUIRE(ai::detail::ModelsTestAccess::replace_models(*models, {}));
    CHECK(models->models().empty());
}

TEST_CASE("ModelStream retains its Models lifetime until terminal delivery", "[ai][models][lifetime]") {
    auto models = std::make_shared<ai::Models>();
    auto transport = successful_transport();
    ai::detail::ModelsTestAccess::set_transport(*models, transport);
    auto stream = models->stream(gateway_model(), {}, {});
    models.reset();
    auto result = tests::run_async_result(std::move(stream).run({}));
    REQUIRE(result);
    CHECK(ai::text_from_assistant_content(result->content) == "hello");
}

TEST_CASE("Models reports model preparation failures without issuing transport requests", "[ai][models][failure]") {
    auto models = std::make_shared<ai::Models>();
    auto transport = successful_transport();
    ai::detail::ModelsTestAccess::set_transport(*models, transport);
    auto invalid = gateway_model();
    invalid.api.clear();
    auto run = tests::run_models(*models, invalid, {}, {});
    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.events.size() == 1);
    CHECK(std::holds_alternative<ai::AssistantErrorEvent>(run.events.front()));
    CHECK(transport->requests.empty());
}

TEST_CASE("Models bounds and redacts gateway terminal diagnostics", "[ai][models][failure][redaction]") {
    auto models = std::make_shared<ai::Models>();
    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .head = {.status_code = 401, .headers = {}},
            .chunks = {"Authorization: Bearer secret-token-value " + std::string(4096, 'x')},
    });
    ai::detail::ModelsTestAccess::set_transport(*models, transport);
    auto run = tests::run_models(*models, gateway_model(), {}, {});
    REQUIRE(run.result);
    REQUIRE(run.result->error_message);
    CHECK(run.result->error_message->size() <= 1024);
    CHECK(run.result->error_message->find("secret-token-value") == std::string::npos);
    REQUIRE_FALSE(run.events.empty());
    const auto* error = std::get_if<ai::AssistantErrorEvent>(&run.events.back());
    REQUIRE(error);
    CHECK(error->error.error_message == run.result->error_message);
    REQUIRE(error->inference_failure);
    CHECK(error->inference_failure->kind == ai::InferenceFailureKind::Unauthorized);
}
