#pragma once

#include <cch/ai/Models.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent {
struct ModelRuntimeTestAccess;

/// Invocation startup inputs. The production endpoint defaults to local Magpie.
struct ModelRuntimeOptions {
    std::filesystem::path agent_dir{};
    std::optional<std::string> api_key{std::nullopt};
    std::string magpie_base_url{};
};

/// Session-facing model policy and diagnostic presentation. Models privately
/// owns gateway discovery, Request Authentication and streaming (ADR 0073).
/// All operations on a runtime and every Session sharing it must be serialized.
class ModelRuntime final : public std::enable_shared_from_this<ModelRuntime> {
public:
    [[nodiscard]] static support::Expected<std::shared_ptr<ModelRuntime>> create(ModelRuntimeOptions options = {});

    /// Test-only inward injection of a composed Models lifetime (ADR 0055).
    explicit ModelRuntime(std::shared_ptr<ai::Models> models);
    ModelRuntime(ModelRuntime&&) noexcept;
    ModelRuntime& operator=(ModelRuntime&&) noexcept;
    ~ModelRuntime();
    ModelRuntime(const ModelRuntime&) = delete;
    ModelRuntime& operator=(const ModelRuntime&) = delete;

    [[nodiscard]] const std::filesystem::path& agent_dir() const noexcept;
    /// Blocking startup discovery; failures clear the catalog and are returned.
    [[nodiscard]] support::ExpectedVoid refresh();
    /// Refresh without blocking the calling executor. Cancellation preserves
    /// the current catalog; discovery failure clears it.
    [[nodiscard]] support::AsyncResult<void> refresh_async(std::stop_token stop_token = {});
    [[nodiscard]] std::optional<std::string> get_error() const;
    [[nodiscard]] support::AsyncResult<ai::GatewayStatus> inspect_gateway(std::stop_token stop_token = {});

    [[nodiscard]] std::vector<ai::Model> models(std::optional<std::string_view> provider_id = std::nullopt) const;
    [[nodiscard]] std::optional<ai::Model> model(std::string_view provider_id, std::string_view model_id) const;
    /// Session Assembly may repeat the bound key but cannot change the identity
    /// of an already used shared Runtime (ADR 0073).
    [[nodiscard]] support::ExpectedVoid configure_startup_key(std::string api_key);
    [[nodiscard]] ai::ModelStreamFactory stream_factory() const;

private:
    struct Impl;
    [[nodiscard]] static std::shared_ptr<ModelRuntime> from_models(
            std::shared_ptr<ai::Models> models, std::filesystem::path agent_dir);
    friend struct ModelRuntimeTestAccess;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::coding_agent
