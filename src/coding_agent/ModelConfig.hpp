#pragma once

#include <cch/ai/Model.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent {

/// One supported `models.json` model definition (pi `ModelDefinitionSchema`
/// subset). The C++ config schema has no compat surface (spec §5 / §9): compat
/// overrides remain an intentional omission.
struct ModelsJsonModel {
    std::string id{};
    std::optional<std::string> name{std::nullopt};
    std::optional<std::string> api{std::nullopt};
    std::optional<std::string> base_url{std::nullopt};
    std::optional<bool> reasoning{std::nullopt};
    std::optional<ai::ThinkingLevelMap> thinking_level_map{std::nullopt};
    std::optional<std::vector<ai::ModelInput>> input{std::nullopt};
    std::optional<ai::ModelCost> cost{std::nullopt};
    std::optional<std::uint64_t> context_window{std::nullopt};
    std::optional<std::uint64_t> max_tokens{std::nullopt};
    std::optional<ai::ModelHeaders> headers{std::nullopt};
};

/// One supported `models.json` model override (pi `ModelOverrideSchema`
/// subset). Applied last, over the composed custom/built-in model with the
/// same id.
struct ModelsJsonModelOverride {
    std::optional<std::string> name{std::nullopt};
    std::optional<bool> reasoning{std::nullopt};
    std::optional<ai::ThinkingLevelMap> thinking_level_map{std::nullopt};
    std::optional<std::vector<ai::ModelInput>> input{std::nullopt};
    std::optional<ai::ModelCost> cost{std::nullopt};
    std::optional<std::uint64_t> context_window{std::nullopt};
    std::optional<std::uint64_t> max_tokens{std::nullopt};
    std::optional<ai::ModelHeaders> headers{std::nullopt};
};

/// One supported `models.json` provider configuration (pi
/// `ProviderConfigSchema` subset). `api_key` is a config value (literal,
/// `$VAR`/`${VAR}` environment template, or `!command`); OAuth definitions
/// (`oauth: "radius"`) are out of the supported subset.
struct ModelsJsonProvider {
    std::optional<std::string> name{std::nullopt};
    std::optional<std::string> base_url{std::nullopt};
    std::optional<std::string> api_key{std::nullopt};
    std::optional<std::string> api{std::nullopt};
    std::optional<ai::ModelHeaders> headers{std::nullopt};
    std::optional<std::vector<ModelsJsonModel>> models{std::nullopt};
    std::optional<std::map<std::string, ModelsJsonModelOverride>> model_overrides{std::nullopt};
};

/// Immutable, credential-blind `models.json` snapshot (pi `ModelConfig`). One
/// load per `refresh()`. Invalid or unreadable content resolves to an empty
/// user config plus a diagnostic string; a missing file resolves to an empty
/// config with no diagnostic.
class ModelConfig {
public:
    ModelConfig() = default;
    ModelConfig(const ModelConfig&) = delete;
    ModelConfig& operator=(const ModelConfig&) = delete;
    ModelConfig(ModelConfig&&) noexcept = default;
    ModelConfig& operator=(ModelConfig&&) noexcept = default;

    /// Synchronous filesystem load (the runtime's refresh is synchronous).
    [[nodiscard]] static ModelConfig load(const std::filesystem::path& path);

    [[nodiscard]] std::optional<ModelsJsonProvider> provider(
        std::string_view provider_id) const;
    [[nodiscard]] std::vector<std::string> provider_ids() const;
    [[nodiscard]] const std::optional<std::string>& error() const { return error_; }
    [[nodiscard]] bool empty() const { return providers_.empty(); }

private:
    explicit ModelConfig(
        std::map<std::string, ModelsJsonProvider, std::less<>> providers,
        std::optional<std::string> error)
        : providers_(std::move(providers)), error_(std::move(error)) {}

    std::map<std::string, ModelsJsonProvider, std::less<>> providers_;
    std::optional<std::string> error_;
};

} // namespace cch::coding_agent
