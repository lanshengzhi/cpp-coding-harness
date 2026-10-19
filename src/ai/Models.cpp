#include <cch/ai/Models.hpp>

#include "ai/MagpieCatalog.hpp"
#include "ai/ModelsImpl.hpp"
#include "ai/ModelsInternal.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/BoostBeastStreamTransport.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace cch::ai {
namespace {

[[nodiscard]] support::Error cancelled_refresh() {
    return support::make_error(support::ErrorCode::Cancelled, "model catalog refresh cancelled");
}

[[nodiscard]] support::Error cancelled_inspect() {
    return support::make_error(support::ErrorCode::Cancelled, "gateway status inspection cancelled");
}

} // namespace

Models::Models(ModelsOptions options) : impl_(std::make_unique<Impl>()) {
    // ADR 0072: gateway keys are invocation state, never persisted credentials.
    impl_->api_key = options.api_key && !options.api_key->empty() ? std::move(*options.api_key)
                     : options.environment_api_key && !options.environment_api_key->empty()
                             ? std::move(*options.environment_api_key)
                             : "magpie";
    impl_->gateway_url = options.gateway_url.empty() ? "http://127.0.0.1:3425" : std::move(options.gateway_url);
    while (impl_->gateway_url.ends_with('/')) {
        impl_->gateway_url.pop_back();
    }
    if (impl_->gateway_url.empty()) {
        impl_->gateway_url = "http://127.0.0.1:3425";
    }
    impl_->transport = std::make_shared<support::BoostBeastStreamTransport>();
}

Models::Models(Models&&) noexcept = default;
Models& Models::operator=(Models&&) noexcept = default;
Models::~Models() = default;

support::ExpectedVoid Models::configure_startup_key(std::string api_key) {
    if (api_key.empty()) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "gateway API key is empty"));
    }
    if (impl_->request_started && api_key != impl_->api_key) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "gateway API key is fixed after the first request; create another model runtime for a different key"));
    }
    impl_->api_key = std::move(api_key);
    return {};
}

support::AsyncResult<void> Models::refresh(std::stop_token stop_token) {
    auto self = shared_from_this();
    return support::detail::make_async_result([self, stop_token]() -> boost::asio::awaitable<support::ExpectedVoid> {
        if (stop_token.stop_requested()) {
            co_return std::unexpected(cancelled_refresh());
        }
        self->impl_->request_started = true;
        auto fetched = co_await fetch_magpie_models(*self->impl_->transport,
                self->impl_->gateway_url + "/v1",
                self->impl_->api_key,
                std::chrono::milliseconds{3000},
                stop_token);
        if (stop_token.stop_requested() || (!fetched && fetched.error().code == support::ErrorCode::Cancelled)) {
            co_return std::unexpected(cancelled_refresh());
        }
        if (fetched) {
            if (auto applied = self->replace_models(std::move(*fetched)); applied) {
                self->impl_->discovery_error.reset();
                co_return support::ExpectedVoid{};
            } else {
                fetched = std::unexpected(applied.error());
            }
        }
        // ADR 0073 extends the approved AI diagnostic policy to discovery.
        auto error = safe_error(std::move(fetched.error()), self->impl_->api_key);
        self->impl_->catalog.clear();
        self->impl_->discovery_error = error;
        co_return std::unexpected(std::move(error));
    });
}

std::optional<support::Error> Models::discovery_error() const { return impl_->discovery_error; }

support::AsyncResult<GatewayStatus> Models::inspect_gateway(std::stop_token stop_token) {
    auto self = shared_from_this();
    return support::detail::make_async_result(
            [self, stop_token]() -> boost::asio::awaitable<support::Expected<GatewayStatus>> {
                if (stop_token.stop_requested()) {
                    co_return std::unexpected(cancelled_inspect());
                }
                GatewayStatus status{.base_url = self->impl_->gateway_url};
                self->impl_->request_started = true;
                auto hello = co_await fetch_magpie_hello(*self->impl_->transport,
                        self->impl_->gateway_url,
                        self->impl_->api_key,
                        std::chrono::milliseconds{3000},
                        stop_token);
                if (stop_token.stop_requested() || (!hello && hello.error().code == support::ErrorCode::Cancelled)) {
                    co_return std::unexpected(cancelled_inspect());
                }
                if (hello) {
                    status.name = std::move(hello->name);
                    status.version = std::move(hello->version);
                } else {
                    status.connection_error = safe_error(std::move(hello.error()), self->impl_->api_key).message;
                }
                auto catalog = co_await fetch_magpie_models(*self->impl_->transport,
                        self->impl_->gateway_url + "/v1",
                        self->impl_->api_key,
                        std::chrono::milliseconds{3000},
                        stop_token);
                if (stop_token.stop_requested() ||
                        (!catalog && catalog.error().code == support::ErrorCode::Cancelled)) {
                    co_return std::unexpected(cancelled_inspect());
                }
                if (catalog) {
                    status.models = std::move(*catalog);
                } else {
                    status.catalog_error = safe_error(std::move(catalog.error()), self->impl_->api_key).message;
                }
                co_return status;
            });
}

support::ExpectedVoid Models::replace_models(std::vector<Model> models) {
    for (const auto& model : models) {
        if (auto valid = validate_model(model); !valid) {
            return std::unexpected(valid.error());
        }
    }
    impl_->catalog = std::move(models);
    return {};
}

std::vector<Model> Models::models(std::optional<std::string_view> provider_id) const {
    if (!provider_id) {
        return impl_->catalog;
    }
    std::vector<Model> result;
    for (const auto& model : impl_->catalog) {
        if (model.provider == *provider_id) {
            result.push_back(model);
        }
    }
    return result;
}

std::optional<Model> Models::model(std::string_view provider_id, std::string_view model_id) const {
    const auto found = std::ranges::find_if(impl_->catalog, [provider_id, model_id](const Model& model) {
        return model.provider == provider_id && model.id == model_id;
    });
    return found == impl_->catalog.end() ? std::nullopt : std::optional<Model>{*found};
}

} // namespace cch::ai
