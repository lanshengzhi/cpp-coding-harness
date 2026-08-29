#include "coding_agent/ModelRuntimeTestSupport.hpp"
#include "coding_agent/ModelRuntimeTransportTestSupport.hpp"

#include "ai/providers/FakeProvider.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/ModelsFixture.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <optional>
#include <utility>

namespace cch::coding_agent {
namespace {

[[nodiscard]] ai::providers::ScriptedProviderDefinition to_scripted_provider(ModelRuntimeTestProvider provider) {
    ai::providers::ScriptedProviderDefinition result;
    result.definition = std::move(provider.definition);
    if (provider.stream) {
        result.stream = [stream = std::move(provider.stream)](
                                ai::Model model, ai::AiContext context, ai::ProviderStreamOptions options) mutable {
            return stream(std::move(model),
                    std::move(context),
                    ModelRuntimeTestStreamOptions{
                            .stop_token = options.stop_token,
                    });
        };
    }
    return result;
}

[[nodiscard]] support::Expected<std::shared_ptr<ModelRuntime>> finish_test_runtime(
        support::Expected<std::shared_ptr<ModelRuntime>> runtime) {
    if (!runtime) {
        return std::unexpected(runtime.error());
    }

    boost::asio::io_context io;
    std::optional<support::Expected<std::vector<ai::Model>>> snapshot;
    boost::asio::co_spawn(
            io,
            [&runtime, &snapshot]() -> boost::asio::awaitable<void> {
                snapshot.emplace(co_await support::detail::await_async_result(runtime.value()->get_available()));
                co_return;
            },
            boost::asio::detached);
    io.run();
    if (!snapshot || !*snapshot) {
        if (snapshot) {
            return std::unexpected(snapshot->error());
        }
        return std::unexpected(support::make_error(
                support::ErrorCode::Auth, "scripted runtime availability snapshot did not complete"));
    }
    return runtime;
}

} // namespace

support::ExpectedVoid apply_model_runtime_test_provider(ai::Models& models, ModelRuntimeTestProvider provider) {
    return ai::providers::apply_scripted_provider(models, to_scripted_provider(std::move(provider)));
}

support::Expected<std::shared_ptr<ModelRuntime>> create_model_runtime_for_testing(
        ModelRuntimeOptions options, ModelRuntimeTestOptions test_options) {
    auto runtime = ModelRuntime::create_impl(std::move(options));
    if (!runtime) {
        return std::unexpected(runtime.error());
    }
    if (!test_options.providers.empty()) {
        (*runtime)->ai_models()->clear_providers();
    }
    for (auto& provider : test_options.providers) {
        if (auto applied = apply_model_runtime_test_provider(*(*runtime)->ai_models(), std::move(provider)); !applied) {
            return std::unexpected(applied.error());
        }
    }
    return finish_test_runtime(std::move(runtime));
}

support::Expected<std::shared_ptr<ModelRuntime>> create_model_runtime_for_testing(
        ModelRuntimeOptions options, ModelRuntimeTransportTestOptions test_options) {
    auto runtime = ModelRuntime::create_impl(std::move(options));
    if (!runtime) {
        return std::unexpected(runtime.error());
    }
    if (auto transports = ai::providers::apply_scripted_transport_options(
                *(*runtime)->ai_models(), std::move(test_options.transports));
            !transports) {
        return std::unexpected(transports.error());
    }
    return finish_test_runtime(std::move(runtime));
}

} // namespace cch::coding_agent

namespace cch::tests {
namespace {

[[nodiscard]] ai::ProviderStreamOptions to_provider_options(coding_agent::ModelRuntimeTestStreamOptions options) {
    ai::ProviderStreamOptions result;
    result.stop_token = options.stop_token;
    return result;
}

[[nodiscard]] coding_agent::ModelRuntimeTestProvider to_runtime_test_provider(
        ai::providers::ScriptedProviderDefinition definition) {
    auto stream = std::move(definition.stream);
    auto provider_definition = std::move(definition.definition);
    coding_agent::ModelRuntimeTestProvider result{
            .definition = std::move(provider_definition),
    };
    if (stream) {
        result.stream = [stream = std::move(stream)](ai::Model model,
                                ai::AiContext context,
                                coding_agent::ModelRuntimeTestStreamOptions options) mutable {
            return stream(std::move(model), std::move(context), to_provider_options(std::move(options)));
        };
    }
    return result;
}

class FakeScriptedProvider final : public ScriptedProvider {
public:
    explicit FakeScriptedProvider(std::string provider_id)
        : ScriptedProvider(provider_id),
          definition_(ai::providers::make_scripted_fake_provider_definition(provider_id)) {
        provider_auth() = std::move(definition_.definition.auth);
    }

    [[nodiscard]] std::string_view name() const noexcept override { return definition_.definition.name; }

    [[nodiscard]] std::vector<ai::Model> models() const override { return definition_.definition.models; }

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext context, coding_agent::ModelRuntimeTestStreamOptions options) override {
        return definition_.stream(std::move(model), std::move(context), to_provider_options(std::move(options)));
    }

private:
    ai::providers::ScriptedProviderDefinition definition_;
};

} // namespace

std::vector<coding_agent::ModelRuntimeTestProvider> make_scripted_fake_provider_definitions() {
    std::vector<coding_agent::ModelRuntimeTestProvider> definitions;
    for (auto&& definition : ai::providers::make_scripted_fake_provider_definitions()) {
        definitions.push_back(to_runtime_test_provider(std::move(definition)));
    }
    return definitions;
}

std::shared_ptr<ScriptedProvider> make_scripted_fake_provider(std::string provider_id) {
    return std::make_shared<FakeScriptedProvider>(std::move(provider_id));
}

std::shared_ptr<ai::Models> make_scripted_fake_models() {
    auto models = std::make_shared<ai::Models>(
            std::make_shared<detail::FixtureCredentialStore>(), std::make_shared<detail::FixtureAuthContext>());
    for (auto&& definition : make_scripted_fake_provider_definitions()) {
        if (auto added = coding_agent::apply_model_runtime_test_provider(*models, std::move(definition)); !added) {
            return nullptr;
        }
    }
    return models;
}

} // namespace cch::tests
