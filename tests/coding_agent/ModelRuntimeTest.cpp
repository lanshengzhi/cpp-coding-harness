#include <cch/coding_agent/ModelRuntime.hpp>

#include "coding_agent/ModelRuntimeTestSupport.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/StreamAdapterFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

using namespace cch;

namespace {

std::shared_ptr<tests::ScriptedTransport> catalog_transport() {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .chunks = {R"({"data":[{"id":"anthropic/claude-x","context_window":200000,"max_output_tokens":8192}]})"},
    });
    transport->repeat_last_attempt = true;
    return transport;
}

} // namespace

TEST_CASE("ModelRuntime ignores malformed and executable model configuration files",
        "[coding_agent][model-runtime][gateway]") {
    for (const auto& config : {std::string{"{malformed"},
                 std::string{
                         R"({"providers":{"direct":{"api":"openai-completions","baseUrl":"https://direct.invalid/v1","apiKey":"!touch marker","models":[{"id":"forbidden"}]}}})"}}) {
        tests::TempWorkspace workspace;
        workspace.write("models.json", config);
        auto transport = catalog_transport();
        auto created = coding_agent::create_model_runtime_for_testing(
                {.agent_dir = workspace.path(), .magpie_base_url = "http://127.0.0.1:34259"}, {}, transport);
        REQUIRE(created);
        auto runtime = *created;
        CHECK_FALSE(runtime->get_error());
        REQUIRE(runtime->models().size() == 1);
        CHECK(runtime->model("magpie", "anthropic/claude-x"));
        CHECK_FALSE(runtime->model("direct", "forbidden"));
        REQUIRE(runtime->refresh());
        CHECK_FALSE(runtime->get_error());
        CHECK_FALSE(std::filesystem::exists(workspace.path() / "auth.json"));
        CHECK_FALSE(std::filesystem::exists(workspace.path() / "marker"));
        CHECK(std::filesystem::exists(workspace.path() / "models.json"));
    }
}

TEST_CASE("ModelRuntime never falls back to static models when gateway discovery fails",
        "[coding_agent][model-runtime][gateway]") {
    tests::TempWorkspace workspace;
    workspace.write("models.json", R"({"providers":{"magpie":{"models":[{"id":"static-model"}]}}})");
    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .failure = support::make_error(support::ErrorCode::Network, "gateway unavailable"),
    });
    auto runtime = coding_agent::create_model_runtime_for_testing({.agent_dir = workspace.path()}, {}, transport);
    REQUIRE(runtime);
    CHECK((*runtime)->models().empty());
    REQUIRE((*runtime)->get_error());
    CHECK((*runtime)->get_error()->find("magpie serve") != std::string::npos);
    CHECK((*runtime)->get_error()->find("/reload") != std::string::npos);
}

TEST_CASE("ModelRuntime cancellation preserves its catalog and failed discovery clears it until recovery",
        "[coding_agent][model-runtime][gateway][cancellation]") {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{.chunks = {R"({"data":[{"id":"old"}]})"}});
    transport->attempts.push_back(tests::TransportAttempt{
            .failure = support::make_error(support::ErrorCode::Network, "gateway unavailable"),
    });
    transport->attempts.push_back(tests::TransportAttempt{.chunks = {R"({"data":[{"id":"new"}]})"}});
    tests::TempWorkspace workspace;
    auto created = coding_agent::create_model_runtime_for_testing({.agent_dir = workspace.path()}, {}, transport);
    REQUIRE(created);
    auto runtime = *created;
    REQUIRE(runtime->model("magpie", "old"));
    std::stop_source stop;
    stop.request_stop();
    auto cancelled = tests::run_async_result(runtime->refresh_async(stop.get_token()));
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == support::ErrorCode::Cancelled);
    CHECK(runtime->model("magpie", "old"));
    CHECK(transport->requests.size() == 1);
    auto failed = tests::run_async_result(runtime->refresh_async());
    REQUIRE_FALSE(failed);
    CHECK(runtime->models().empty());
    REQUIRE(runtime->get_error());
    CHECK(runtime->get_error()->find("gateway unavailable") != std::string::npos);
    REQUIRE(tests::run_async_result(runtime->refresh_async()));
    CHECK(runtime->model("magpie", "new"));
    CHECK_FALSE(runtime->get_error());
}

TEST_CASE("ModelRuntime binds invocation key precedence without persistent model credentials",
        "[coding_agent][model-runtime][gateway]") {
    tests::EnvVarGuard key{"MAGPIE_API_KEY"};
    key.set("environment-key");
    for (const bool explicit_override : {false, true}) {
        tests::TempWorkspace workspace;
        auto wire = catalog_transport();
        wire->attempts.push_back(tests::TransportAttempt{
                .chunks = {"data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}\n\n"},
        });
        auto created = coding_agent::create_model_runtime_for_testing(
                {.agent_dir = workspace.path(),
                        .api_key = explicit_override ? std::optional<std::string>{"invocation-key"} : std::nullopt},
                {},
                wire);
        REQUIRE(created);
        auto runtime = *created;
        auto model = runtime->model("magpie", "anthropic/claude-x");
        REQUIRE(model);
        auto stream = runtime->stream_factory()(*model, {}, {});
        runtime.reset();
        auto result = tests::run_async_result(std::move(stream).run({}));
        REQUIRE(result);
        CHECK(result->stop_reason == ai::AssistantStopReason::Stop);
        REQUIRE(wire->requests.size() == 2);
        for (const auto& request : wire->requests) {
            CHECK(request.headers.at("Authorization") ==
                    (explicit_override ? "Bearer invocation-key" : "Bearer environment-key"));
        }
        CHECK_FALSE(std::filesystem::exists(workspace.path() / "auth.json"));
    }
}

TEST_CASE("ModelRuntime inspect_gateway forwards status and honors stop_token",
        "[coding_agent][model-runtime][gateway][cancellation]") {
    tests::TempWorkspace workspace;
    auto wire = catalog_transport();
    auto created = coding_agent::create_model_runtime_for_testing(
            {.agent_dir = workspace.path(), .magpie_base_url = "http://127.0.0.1:34259"}, {}, wire);
    REQUIRE(created);
    auto runtime = *created;

    std::stop_source stopped;
    stopped.request_stop();
    auto cancelled = tests::run_async_result(runtime->inspect_gateway(stopped.get_token()));
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == support::ErrorCode::Cancelled);

    wire->attempts.push_back(tests::TransportAttempt{
            .chunks = {R"({"name":"magpie-srv","version":"2.0"})"},
    });
    wire->attempts.push_back(tests::TransportAttempt{
            .chunks = {R"({"data":[{"id":"runtime-model"}]})"},
    });
    auto status = tests::run_async_result(runtime->inspect_gateway());
    REQUIRE(status);
    CHECK(status->name == "magpie-srv");
    CHECK(status->version == "2.0");
    REQUIRE(status->models.size() == 1);
    CHECK(status->models.front().id == "runtime-model");
}
