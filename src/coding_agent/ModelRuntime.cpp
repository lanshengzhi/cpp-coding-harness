#include <cch/coding_agent/ModelRuntime.hpp>

#include "coding_agent/GatewayOptions.hpp"
#include <cch/coding_agent/AgentConfigDir.hpp>

#include "support/AsyncResultBridge.hpp"

#include <boost/asio/io_context.hpp>

#include <cstdlib>
#include <exception>
#include <utility>

namespace cch::coding_agent {
namespace {

[[nodiscard]] std::string catalog_diagnostic(std::string_view error) {
    return "Magpie model discovery failed: " + std::string{error} +
           ". Start the local gateway with `magpie serve`, then run `/reload`.";
}

} // namespace

struct ModelRuntime::Impl {
    std::filesystem::path agent_dir;
    std::shared_ptr<ai::Models> models;
    bool injected_catalog{false};
};

ModelRuntime::ModelRuntime(std::shared_ptr<ai::Models> models) : impl_(std::make_unique<Impl>()) {
    if (!models) {
        std::terminate();
    }
    impl_->models = std::move(models);
    impl_->injected_catalog = true;
}

std::shared_ptr<ModelRuntime> ModelRuntime::from_models(
        std::shared_ptr<ai::Models> models, std::filesystem::path agent_dir) {
    auto runtime = std::make_shared<ModelRuntime>(std::move(models));
    runtime->impl_->agent_dir = std::move(agent_dir);
    runtime->impl_->injected_catalog = false;
    return runtime;
}

ModelRuntime::ModelRuntime(ModelRuntime&&) noexcept = default;
ModelRuntime& ModelRuntime::operator=(ModelRuntime&&) noexcept = default;
ModelRuntime::~ModelRuntime() = default;

support::Expected<std::shared_ptr<ModelRuntime>> ModelRuntime::create(ModelRuntimeOptions options) {
    auto models = std::make_shared<ai::Models>(make_gateway_models_options(options.api_key, options.magpie_base_url));
    auto runtime = from_models(
            std::move(models), options.agent_dir.empty() ? agent_config_dir() : std::move(options.agent_dir));
    // Discovery failure remains a visible runtime diagnostic, allowing /reload.
    static_cast<void>(runtime->refresh());
    return runtime;
}

const std::filesystem::path& ModelRuntime::agent_dir() const noexcept {
    return impl_->agent_dir;
}

support::ExpectedVoid ModelRuntime::refresh() {
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> outcome;
    // outcome and io live until io.run drains the pending operation.
    auto pending = support::detail::make_async_result_on(io.get_executor(),
            [models = impl_->models,
                    injected = impl_->injected_catalog]() -> boost::asio::awaitable<support::ExpectedVoid> {
                if (injected) co_return support::ExpectedVoid{};
                co_return co_await support::detail::await_async_result(models->refresh());
            });
    std::move(pending).start([&outcome](support::ExpectedVoid result) noexcept { outcome.emplace(std::move(result)); });
    io.run();
    if (!outcome) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Provider, "Magpie catalog discovery did not complete"));
    }
    if (!*outcome) {
        auto error = std::move(outcome->error());
        error.message = catalog_diagnostic(error.message);
        return std::unexpected(std::move(error));
    }
    return {};
}

support::AsyncResult<void> ModelRuntime::refresh_async(std::stop_token stop_token) {
    const auto self = weak_from_this().lock();
    if (!self) {
        return support::AsyncResult<void>(std::unexpected(support::make_error(
                support::ErrorCode::Validation, "asynchronous model runtime refresh requires shared ownership")));
    }
    return support::detail::make_async_result([self, stop_token]() -> boost::asio::awaitable<support::ExpectedVoid> {
        if (stop_token.stop_requested()) {
            co_return std::unexpected(
                    support::make_error(support::ErrorCode::Cancelled, "model catalog refresh cancelled"));
        }
        if (self->impl_->injected_catalog) co_return support::ExpectedVoid{};
        auto result = co_await support::detail::await_async_result(self->impl_->models->refresh(stop_token));
        if (!result && result.error().code != support::ErrorCode::Cancelled) {
            auto error = std::move(result.error());
            error.message = catalog_diagnostic(error.message);
            co_return std::unexpected(std::move(error));
        }
        co_return result;
    });
}

std::optional<std::string> ModelRuntime::get_error() const {
    const auto error = impl_->models->discovery_error();
    return error ? std::optional<std::string>{catalog_diagnostic(error->message)} : std::nullopt;
}

std::vector<ai::Model> ModelRuntime::models(std::optional<std::string_view> provider_id) const {
    return impl_->models->models(provider_id);
}

std::optional<ai::Model> ModelRuntime::model(std::string_view provider_id, std::string_view model_id) const {
    return impl_->models->model(provider_id, model_id);
}

support::ExpectedVoid ModelRuntime::configure_startup_key(std::string api_key) {
    return impl_->models->configure_startup_key(std::move(api_key));
}

ai::ModelStreamFactory ModelRuntime::stream_factory() const {
    return [models = impl_->models](ai::Model model, ai::AiContext context, ai::SimpleStreamOptions options) {
        return models->stream(std::move(model), std::move(context), std::move(options));
    };
}

support::AsyncResult<ai::GatewayStatus> ModelRuntime::inspect_gateway(std::stop_token stop_token) {
    return impl_->models->inspect_gateway(std::move(stop_token));
}

} // namespace cch::coding_agent
