#include "coding_agent/ModelRuntimeTestSupport.hpp"

#include "coding_agent/GatewayOptions.hpp"
#include "coding_agent/ModelRuntimeTestAccess.hpp"
#include "ai/ModelsTestAccess.hpp"
#include <cch/coding_agent/AgentConfigDir.hpp>
#include "support/ScriptedProvider.hpp"
#include "support/StreamAdapterFixture.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/ModelsFixture.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <optional>
#include <cstdlib>
#include <utility>

namespace cch::coding_agent {
namespace {

[[nodiscard]] tests::ScriptedProviderDefinition to_scripted_provider(ModelRuntimeTestProvider provider) {
    tests::ScriptedProviderDefinition result;
    result.definition = std::move(provider.definition);
    if (provider.stream) {
        result.stream = [stream = std::move(provider.stream)](
                                ai::Model model, ai::AiContext context, ai::SimpleStreamOptions options) mutable {
            return stream(std::move(model),
                    std::move(context),
                    ModelRuntimeTestStreamOptions{
                            .stop_token = options.stop_token,
                    });
        };
    }
    return result;
}

} // namespace

support::ExpectedVoid apply_model_runtime_test_provider(ai::Models& models, ModelRuntimeTestProvider provider) {
    return tests::apply_scripted_provider(models, to_scripted_provider(std::move(provider)));
}

support::Expected<std::shared_ptr<ModelRuntime>> create_model_runtime_for_testing(ModelRuntimeOptions options,
        ModelRuntimeTestOptions test_options,
        std::shared_ptr<support::StreamTransport> catalog_transport) {
    if (options.magpie_base_url.empty()) {
        options.magpie_base_url = "http://127.0.0.1:34259";
    }
    if (!catalog_transport) {
        catalog_transport = make_empty_magpie_transport();
    }
    auto models = std::make_shared<ai::Models>(
            make_gateway_models_options(std::move(options.api_key), std::move(options.magpie_base_url)));
    ai::detail::ModelsTestAccess::set_transport(*models, std::move(catalog_transport));
    auto runtime = ModelRuntimeTestAccess::create(
            models, options.agent_dir.empty() ? agent_config_dir() : std::move(options.agent_dir));
    static_cast<void>(runtime->refresh());
    if (!test_options.providers.empty()) {
        if (auto cleared = ai::detail::ModelsTestAccess::replace_models(*models, {}); !cleared)
            return std::unexpected(cleared.error());
    }
    for (auto& provider : test_options.providers) {
        if (auto applied = apply_model_runtime_test_provider(*models, std::move(provider)); !applied) {
            return std::unexpected(applied.error());
        }
    }
    if (auto applied = tests::apply_scripted_transport_options(*models, std::move(test_options.transports)); !applied) {
        return std::unexpected(applied.error());
    }
    return runtime;
}

std::shared_ptr<support::StreamTransport> make_empty_magpie_transport() {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .head = {.status_code = 200, .headers = {}},
            .chunks = {R"({"data":[]})"},
            .failure = std::nullopt,
    });
    transport->repeat_last_attempt = true;
    return transport;
}

} // namespace cch::coding_agent

namespace cch::tests {
namespace {

[[nodiscard]] ai::SimpleStreamOptions to_simple_options(coding_agent::ModelRuntimeTestStreamOptions options) {
    ai::SimpleStreamOptions result;
    result.stop_token = options.stop_token;
    return result;
}

[[nodiscard]] coding_agent::ModelRuntimeTestProvider to_runtime_test_provider(ScriptedProviderDefinition definition) {
    auto stream = std::move(definition.stream);
    auto provider_definition = std::move(definition.definition);
    coding_agent::ModelRuntimeTestProvider result{
            .definition = std::move(provider_definition),
    };
    if (stream) {
        result.stream = [stream = std::move(stream)](ai::Model model,
                                ai::AiContext context,
                                coding_agent::ModelRuntimeTestStreamOptions options) mutable {
            return stream(std::move(model), std::move(context), to_simple_options(std::move(options)));
        };
    }
    return result;
}

class FakeScriptedProvider final : public ScriptedProvider {
public:
    explicit FakeScriptedProvider(std::string provider_id)
        : ScriptedProvider(provider_id), definition_(make_ai_scripted_fake_provider_definition(provider_id)) {}

    [[nodiscard]] std::string_view name() const noexcept override { return definition_.definition.name; }

    [[nodiscard]] std::vector<ai::Model> models() const override { return definition_.definition.models; }

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext context, coding_agent::ModelRuntimeTestStreamOptions options) override {
        return definition_.stream(std::move(model), std::move(context), to_simple_options(std::move(options)));
    }

private:
    ScriptedProviderDefinition definition_;
};

} // namespace

std::vector<coding_agent::ModelRuntimeTestProvider> make_scripted_fake_provider_definitions() {
    std::vector<coding_agent::ModelRuntimeTestProvider> definitions;
    for (auto&& definition : make_ai_scripted_fake_provider_definitions()) {
        definitions.push_back(to_runtime_test_provider(std::move(definition)));
    }
    return definitions;
}

std::shared_ptr<ScriptedProvider> make_scripted_fake_provider(std::string provider_id) {
    return std::make_shared<FakeScriptedProvider>(std::move(provider_id));
}

std::shared_ptr<ai::Models> make_scripted_fake_models() {
    auto models = std::make_shared<ai::Models>();
    for (auto&& definition : make_scripted_fake_provider_definitions()) {
        if (auto added = coding_agent::apply_model_runtime_test_provider(*models, std::move(definition)); !added) {
            return nullptr;
        }
    }
    return models;
}

} // namespace cch::tests
