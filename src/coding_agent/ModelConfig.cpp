#include "ModelConfig.hpp"

#include "util/Json.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace cch::coding_agent {
namespace {

using cch::util::JsonValue;

[[nodiscard]] std::string dotted_path(std::string path, std::string_view child) {
    if (path.empty()) {
        return std::string{child};
    }
    return path + "." + std::string{child};
}

class ConfigValidator {
public:
    explicit ConfigValidator(std::string root) : root_(std::move(root)) {}

    [[nodiscard]] bool has_errors() const { return !errors_.empty(); }
    [[nodiscard]] std::string error_text() const {
        std::string text;
        for (const auto& error : errors_) {
            if (!text.empty()) {
                text += "\n";
            }
            text += "  - " + error;
        }
        return text;
    }

    void error(std::string path, std::string message) {
        errors_.push_back(dotted_path(root_, path) + ": " + std::move(message));
    }

    /// Returns the value if it is the expected type, otherwise records an
    /// error and returns nullptr.
    [[nodiscard]] const JsonValue::object_t* expect_object(
        const JsonValue& value,
        std::string path) {
        const auto* object = value.get_if<JsonValue::object_t>();
        if (object == nullptr) {
            error(std::move(path), "Expected object");
        }
        return object;
    }

    [[nodiscard]] const JsonValue::array_t* expect_array(
        const JsonValue& value,
        std::string path) {
        const auto* array = value.get_if<JsonValue::array_t>();
        if (array == nullptr) {
            error(std::move(path), "Expected array");
        }
        return array;
    }

    [[nodiscard]] std::optional<std::string> optional_string(
        const JsonValue::object_t& object,
        std::string_view key,
        std::string path) {
        const auto found = object.find(std::string{key});
        if (found == object.end()) {
            return std::nullopt;
        }
        const auto* text = found->second.get_if<std::string>();
        if (text == nullptr) {
            error(std::move(path), "Expected string");
            return std::nullopt;
        }
        if (text->empty()) {
            error(std::move(path), "Expected string length >= 1");
            return std::nullopt;
        }
        return *text;
    }

    std::optional<bool> optional_bool(
        const JsonValue::object_t& object,
        std::string_view key,
        std::string path) {
        const auto found = object.find(std::string{key});
        if (found == object.end()) {
            return std::nullopt;
        }
        const auto* flag = found->second.get_if<bool>();
        if (flag == nullptr) {
            error(std::move(path), "Expected boolean");
            return std::nullopt;
        }
        return *flag;
    }

    std::optional<double> optional_number(
        const JsonValue::object_t& object,
        std::string_view key,
        std::string path) {
        const auto found = object.find(std::string{key});
        if (found == object.end()) {
            return std::nullopt;
        }
        const auto* number = found->second.get_if<double>();
        if (number == nullptr) {
            error(std::move(path), "Expected number");
            return std::nullopt;
        }
        return *number;
    }

    std::optional<ai::ModelHeaders> optional_string_map(
        const JsonValue::object_t& object,
        std::string_view key,
        std::string path) {
        const auto found = object.find(std::string{key});
        if (found == object.end()) {
            return std::nullopt;
        }
        const auto* entries = expect_object(found->second, std::move(path));
        if (entries == nullptr) {
            return std::nullopt;
        }
        ai::ModelHeaders result;
        for (const auto& [name, value] : *entries) {
            const auto* text = value.get_if<std::string>();
            if (text == nullptr) {
                error(name, "Expected string");
                continue;
            }
            result.emplace(name, *text);
        }
        return result;
    }

    std::optional<ai::ThinkingLevelMap> optional_thinking_level_map(
        const JsonValue::object_t& object,
        std::string_view key,
        std::string path) {
        const auto found = object.find(std::string{key});
        if (found == object.end()) {
            return std::nullopt;
        }
        const auto* entries = expect_object(found->second, std::move(path));
        if (entries == nullptr) {
            return std::nullopt;
        }
        ai::ThinkingLevelMap result;
        const auto parse_level = [&](ai::ModelThinkingLevel level) {
            const auto it = entries->find(std::string{level_name(level)});
            if (it == entries->end()) {
                return;
            }
            if (it->second.holds<JsonValue::null_t>()) {
                result.emplace(level, std::nullopt);
                return;
            }
            const auto* text = it->second.get_if<std::string>();
            if (text == nullptr) {
                error(std::string{level_name(level)}, "Expected string or null");
                return;
            }
            result.emplace(level, *text);
        };
        parse_level(ai::ModelThinkingLevel::Off);
        parse_level(ai::ModelThinkingLevel::Minimal);
        parse_level(ai::ModelThinkingLevel::Low);
        parse_level(ai::ModelThinkingLevel::Medium);
        parse_level(ai::ModelThinkingLevel::High);
        parse_level(ai::ModelThinkingLevel::XHigh);
        parse_level(ai::ModelThinkingLevel::Max);
        return result;
    }

    std::optional<std::vector<ai::ModelInput>> optional_input(
        const JsonValue::object_t& object,
        std::string_view key,
        std::string path) {
        const auto found = object.find(std::string{key});
        if (found == object.end()) {
            return std::nullopt;
        }
        const auto* array = expect_array(found->second, std::move(path));
        if (array == nullptr) {
            return std::nullopt;
        }
        std::vector<ai::ModelInput> result;
        for (std::size_t index = 0; index < array->size(); ++index) {
            const auto& entry = (*array)[index];
            const auto* text = entry.get_if<std::string>();
            if (text == nullptr) {
                error(path + "[" + std::to_string(index) + "]", "Expected string");
                continue;
            }
            if (*text == "text") {
                result.push_back(ai::ModelInput::Text);
            } else if (*text == "image") {
                result.push_back(ai::ModelInput::Image);
            } else {
                error(
                    path + "[" + std::to_string(index) + "]",
                    "Expected 'text' or 'image'");
            }
        }
        return result;
    }

    std::optional<ai::ModelCost> optional_cost(
        const JsonValue::object_t& object,
        std::string_view key,
        std::string path) {
        const auto found = object.find(std::string{key});
        if (found == object.end()) {
            return std::nullopt;
        }
        const auto* cost_object = expect_object(found->second, std::move(path));
        if (cost_object == nullptr) {
            return std::nullopt;
        }
        ai::ModelCost cost;
        cost.input = required_rate(*cost_object, "input", path);
        cost.output = required_rate(*cost_object, "output", path);
        cost.cache_read = required_rate(*cost_object, "cacheRead", path);
        cost.cache_write = required_rate(*cost_object, "cacheWrite", path);
        const auto tiers = cost_object->find("tiers");
        if (tiers != cost_object->end()) {
            const auto* tier_array = expect_array(
                tiers->second, dotted_path(path, "tiers"));
            if (tier_array != nullptr) {
                std::vector<ai::ModelCostTier> parsed_tiers;
                for (std::size_t index = 0; index < tier_array->size(); ++index) {
                    const auto tier_path = dotted_path(
                        dotted_path(path, "tiers"), "[" + std::to_string(index) + "]");
                    const auto* tier_object = expect_object(
                        (*tier_array)[index], tier_path);
                    if (tier_object == nullptr) {
                        continue;
                    }
                    ai::ModelCostTier tier;
                    tier.input = required_rate(*tier_object, "input", tier_path);
                    tier.output = required_rate(*tier_object, "output", tier_path);
                    tier.cache_read = required_rate(*tier_object, "cacheRead", tier_path);
                    tier.cache_write = required_rate(*tier_object, "cacheWrite", tier_path);
                    const auto above = tier_object->find("inputTokensAbove");
                    if (above != tier_object->end()) {
                        const auto* number = above->second.get_if<double>();
                        if (number == nullptr) {
                            error(dotted_path(tier_path, "inputTokensAbove"), "Expected number");
                        } else {
                            tier.input_tokens_above = static_cast<std::uint64_t>(*number);
                        }
                    }
                    parsed_tiers.push_back(std::move(tier));
                }
                cost.tiers = std::move(parsed_tiers);
            }
        }
        return cost;
    }

private:
    [[nodiscard]] static std::string_view level_name(ai::ModelThinkingLevel level) {
        switch (level) {
        case ai::ModelThinkingLevel::Off:
            return "off";
        case ai::ModelThinkingLevel::Minimal:
            return "minimal";
        case ai::ModelThinkingLevel::Low:
            return "low";
        case ai::ModelThinkingLevel::Medium:
            return "medium";
        case ai::ModelThinkingLevel::High:
            return "high";
        case ai::ModelThinkingLevel::XHigh:
            return "xhigh";
        case ai::ModelThinkingLevel::Max:
            return "max";
        }
        return "off";
    }

    double required_rate(
        const JsonValue::object_t& object,
        std::string_view key,
        const std::string& path) {
        const auto found = object.find(std::string{key});
        if (found == object.end()) {
            error(dotted_path(path, key), "Required");
            return 0;
        }
        const auto* number = found->second.get_if<double>();
        if (number == nullptr) {
            error(dotted_path(path, key), "Expected number");
            return 0;
        }
        return *number;
    }

    std::string root_;
    std::vector<std::string> errors_;
};

[[nodiscard]] bool read_file_content(
    const std::filesystem::path& path,
    std::optional<std::string>& error,
    std::string& content) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        if (std::filesystem::exists(path)) {
            error = "Failed to load models.json\n\nFile: " + path.string();
        }
        return false;
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    content = buffer.str();
    return true;
}

[[nodiscard]] std::optional<std::uint64_t> to_uint64(std::optional<double> value) {
    if (!value) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(*value);
}

[[nodiscard]] ModelsJsonModel parse_model(
    ConfigValidator& validator,
    const JsonValue& value,
    const std::string& path) {
    ModelsJsonModel model;
    const auto* object = validator.expect_object(value, path);
    if (object == nullptr) {
        return model;
    }
    const auto id_path = dotted_path(path, "id");
    const auto id_value = object->find("id");
    if (id_value == object->end()) {
        validator.error(id_path, "Required");
    } else if (const auto id = id_value->second.get_if<std::string>();
               id == nullptr) {
        validator.error(id_path, "Expected string");
    } else if (id->empty()) {
        validator.error(id_path, "Expected string length >= 1");
    } else {
        model.id = *id;
    }
    model.name = validator.optional_string(*object, "name", dotted_path(path, "name"));
    model.api = validator.optional_string(*object, "api", dotted_path(path, "api"));
    model.base_url = validator.optional_string(*object, "baseUrl", dotted_path(path, "baseUrl"));
    model.reasoning = validator.optional_bool(*object, "reasoning", dotted_path(path, "reasoning"));
    model.thinking_level_map = validator.optional_thinking_level_map(
        *object, "thinkingLevelMap", dotted_path(path, "thinkingLevelMap"));
    model.input = validator.optional_input(*object, "input", dotted_path(path, "input"));
    model.cost = validator.optional_cost(*object, "cost", dotted_path(path, "cost"));
    model.context_window = to_uint64(
        validator.optional_number(*object, "contextWindow", dotted_path(path, "contextWindow")));
    model.max_tokens = to_uint64(
        validator.optional_number(*object, "maxTokens", dotted_path(path, "maxTokens")));
    model.headers = validator.optional_string_map(*object, "headers", dotted_path(path, "headers"));
    return model;
}

[[nodiscard]] ModelsJsonModelOverride parse_model_override(
    ConfigValidator& validator,
    const JsonValue& value,
    const std::string& path) {
    ModelsJsonModelOverride override_value;
    const auto* object = validator.expect_object(value, path);
    if (object == nullptr) {
        return override_value;
    }
    override_value.name = validator.optional_string(*object, "name", dotted_path(path, "name"));
    override_value.reasoning = validator.optional_bool(
        *object, "reasoning", dotted_path(path, "reasoning"));
    override_value.thinking_level_map = validator.optional_thinking_level_map(
        *object, "thinkingLevelMap", dotted_path(path, "thinkingLevelMap"));
    override_value.input = validator.optional_input(
        *object, "input", dotted_path(path, "input"));
    override_value.cost = validator.optional_cost(*object, "cost", dotted_path(path, "cost"));
    override_value.context_window = to_uint64(
        validator.optional_number(*object, "contextWindow", dotted_path(path, "contextWindow")));
    override_value.max_tokens = to_uint64(
        validator.optional_number(*object, "maxTokens", dotted_path(path, "maxTokens")));
    override_value.headers = validator.optional_string_map(
        *object, "headers", dotted_path(path, "headers"));
    return override_value;
}

[[nodiscard]] ModelsJsonProvider parse_provider(
    ConfigValidator& validator,
    const JsonValue& value,
    const std::string& path) {
    ModelsJsonProvider provider;
    const auto* object = validator.expect_object(value, path);
    if (object == nullptr) {
        return provider;
    }
    provider.name = validator.optional_string(*object, "name", dotted_path(path, "name"));
    provider.base_url = validator.optional_string(*object, "baseUrl", dotted_path(path, "baseUrl"));
    provider.api_key = validator.optional_string(*object, "apiKey", dotted_path(path, "apiKey"));
    provider.api = validator.optional_string(*object, "api", dotted_path(path, "api"));
    provider.headers = validator.optional_string_map(
        *object, "headers", dotted_path(path, "headers"));

    if (const auto found = object->find("models"); found != object->end()) {
        const auto models_path = dotted_path(path, "models");
        const auto* array = validator.expect_array(found->second, models_path);
        if (array != nullptr) {
            std::vector<ModelsJsonModel> models;
            for (std::size_t index = 0; index < array->size(); ++index) {
                models.push_back(parse_model(
                    validator,
                    (*array)[index],
                    models_path + "[" + std::to_string(index) + "]"));
            }
            provider.models = std::move(models);
        }
    }

    if (const auto found = object->find("modelOverrides"); found != object->end()) {
        const auto overrides_path = dotted_path(path, "modelOverrides");
        const auto* overrides = validator.expect_object(found->second, overrides_path);
        if (overrides != nullptr) {
            std::map<std::string, ModelsJsonModelOverride> parsed_overrides;
            for (const auto& [model_id, value] : *overrides) {
                parsed_overrides.emplace(
                    model_id,
                    parse_model_override(
                        validator, value, overrides_path + "." + model_id));
            }
            provider.model_overrides = std::move(parsed_overrides);
        }
    }
    return provider;
}

} // namespace

ModelConfig ModelConfig::load(const std::filesystem::path& path) {
    if (path.empty()) {
        return ModelConfig({}, std::nullopt);
    }

    std::optional<std::string> load_error;
    std::string content;
    if (!read_file_content(path, load_error, content)) {
        return ModelConfig({}, std::move(load_error));
    }
    if (content.empty()) {
        // Empty content parses as an empty config (pi's stripJsonComments then
        // JSON.parse of "" fails; treat it as invalid rather than empty).
        return ModelConfig({}, "Failed to parse models.json: Unexpected end of JSON input\n\nFile: " + path.string());
    }

    auto parsed = util::read_json<util::JsonValue>(content);
    if (!parsed) {
        return ModelConfig(
            {},
            "Failed to parse models.json: " + parsed.error().message +
                "\n\nFile: " + path.string());
    }

    const auto* root = parsed->get_if<util::JsonValue::object_t>();
    if (root == nullptr) {
        return ModelConfig(
            {},
            "Invalid models.json schema:\n  - root: Expected object\n\nFile: " +
                path.string());
    }
    const auto providers_found = root->find("providers");
    if (providers_found == root->end()) {
        return ModelConfig(
            {},
            "Invalid models.json schema:\n  - providers: Required\n\nFile: " +
                path.string());
    }

    ConfigValidator validator{"providers"};
    std::map<std::string, ModelsJsonProvider, std::less<>> providers;
    const auto* providers_object = providers_found->second.get_if<util::JsonValue::object_t>();
    if (providers_object == nullptr) {
        validator.error("", "Expected object");
    } else {
        for (const auto& [provider_id, value] : *providers_object) {
            auto provider = parse_provider(
                validator, value, dotted_path("", provider_id));
            providers.emplace(provider_id, std::move(provider));
        }
    }

    if (validator.has_errors()) {
        return ModelConfig(
            {},
            "Invalid models.json schema:\n" + validator.error_text() +
                "\n\nFile: " + path.string());
    }
    return ModelConfig(std::move(providers), std::nullopt);
}

std::optional<ModelsJsonProvider> ModelConfig::provider(
    std::string_view provider_id) const {
    const auto found = providers_.find(provider_id);
    if (found == providers_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::vector<std::string> ModelConfig::provider_ids() const {
    std::vector<std::string> ids;
    ids.reserve(providers_.size());
    for (const auto& [id, _] : providers_) {
        ids.push_back(id);
    }
    return ids;
}

} // namespace cch::coding_agent
