#pragma once

#include <cch/ai/Context.hpp>
#include <cch/ai/ModelStream.hpp>
#include <cch/ai/RequestOptions.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/AsyncResult.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <stop_token>
#include <vector>

namespace cch::ai {
namespace detail {
struct ModelsTestAccess;
}

/// Trusted startup inputs for gateway Request Authentication (ADR 0072).
/// The host supplies ambient environment state explicitly; Models owns key
/// precedence and never exposes the effective key through catalog values.
struct ModelsOptions {
    std::optional<std::string> api_key{std::nullopt};
    std::optional<std::string> environment_api_key{std::nullopt};
    std::string gateway_url{"http://127.0.0.1:3425"};
};

/// Passive gateway facts, never a request-authentication or availability value.
struct GatewayStatus {
    std::string base_url{};
    std::optional<std::string> name{std::nullopt};
    std::optional<std::string> version{std::nullopt};
    std::vector<Model> models{};
    std::optional<std::string> connection_error{std::nullopt};
    std::optional<std::string> catalog_error{std::nullopt};
};

/// Gateway model catalog and one-turn inference. All operations on a shared
/// Models value must be serialized by its calling executor (ADR 0040).
class Models final : public std::enable_shared_from_this<Models> {
public:
    explicit Models(ModelsOptions options = {});
    Models(Models&&) noexcept;
    Models& operator=(Models&&) noexcept;
    ~Models();
    Models(const Models&) = delete;
    Models& operator=(const Models&) = delete;

    /// Bind an explicit startup override before the first gateway request.
    /// Repeating the same key is harmless for shared Session assembly; changing
    /// it after any gateway request is rejected. No key is persisted or returned.
    [[nodiscard]] support::ExpectedVoid configure_startup_key(std::string api_key);

    /// Refresh atomically; failure clears the catalog, cancellation preserves it.
    /// Pending operations retain Models and run on the consuming executor.
    [[nodiscard]] support::AsyncResult<void> refresh(std::stop_token stop_token = {});
    [[nodiscard]] std::optional<support::Error> discovery_error() const;
    /// Inspect hello and catalog independently without changing the active catalog.
    [[nodiscard]] support::AsyncResult<GatewayStatus> inspect_gateway(std::stop_token stop_token = {});
    [[nodiscard]] std::vector<Model> models(std::optional<std::string_view> provider_id = std::nullopt) const;
    [[nodiscard]] std::optional<Model> model(std::string_view provider_id, std::string_view model_id) const;

    /// Produce a lazy, move-only stream retaining the Models lifetime. Domain
    /// failures deliver a terminal event and AssistantMessage; sink and
    /// infrastructure failures use the Expected error alternative.
    [[nodiscard]] ModelStream stream(Model model, AiContext context, SimpleStreamOptions options);

    struct Impl;

private:
    friend struct detail::ModelsTestAccess;
    [[nodiscard]] support::ExpectedVoid replace_models(std::vector<Model> models);
    std::unique_ptr<Impl> impl_;
};

using ModelStreamFactory = std::move_only_function<ModelStream(Model, AiContext, SimpleStreamOptions)>;

} // namespace cch::ai
