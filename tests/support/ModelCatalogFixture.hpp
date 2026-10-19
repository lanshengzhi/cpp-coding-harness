#pragma once

#include <cch/ai/Models.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/support/JsonAccess.hpp>

#include "ai/ModelsTestAccess.hpp"
#include "support/Json.hpp"
#include "support/ModelFixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tests {

/// Explicit passive catalog injection for frozen Session/TUI fixtures. This
/// reads only the supplied fixture text, never files, credentials or key
/// commands. Its grouped JSON records preserve historical model identities;
/// production model configuration is not involved (ADR 0072).
struct ModelCatalogFixture {
    std::shared_ptr<ai::Models> models{std::make_shared<ai::Models>()};
    std::shared_ptr<coding_agent::ModelRuntime> runtime{std::make_shared<coding_agent::ModelRuntime>(models)};

    void set(std::string_view text) {
        auto parsed = support::read_json(text);
        REQUIRE(parsed);
        const auto* root = support::json_object(*parsed);
        REQUIRE(root);
        const auto* groups = support::json_object_member(*root, "providers");
        std::vector<ai::Model> catalog;
        if (groups) {
            for (const auto& [identity, value] : *groups) {
                const auto* group = support::json_object(value);
                REQUIRE(group);
                const auto* entries = support::json_array_member(*group, "models");
                if (!entries) continue;
                for (const auto& entry : *entries) {
                    const auto* item = support::json_object(entry);
                    REQUIRE(item);
                    const auto id = support::json_string_member(*item, "id");
                    REQUIRE(id);
                    auto model = make_model(std::string{*id},
                            identity,
                            std::string{support::json_string_member(*item, "api")
                                            .value_or(support::json_string_member(*group, "api")
                                                            .value_or("openai-completions"))});
                    model.name = std::string{support::json_string_member(*item, "name").value_or(*id)};
                    model.reasoning = support::json_bool_member(*item, "reasoning").value_or(false);
                    model.context_window = static_cast<std::uint64_t>(
                            support::json_integer_member(*item, "contextWindow").value_or(128000));
                    model.max_tokens = static_cast<std::uint64_t>(
                            support::json_integer_member(*item, "maxTokens").value_or(16384));
                    if (const auto* inputs = support::json_array_member(*item, "input")) {
                        model.input.clear();
                        for (const auto& input : *inputs) {
                            if (const auto* name = input.get_if<std::string>()) {
                                model.input.push_back(*name == "image" ? ai::ModelInput::Image : ai::ModelInput::Text);
                            }
                        }
                    }
                    if (const auto* levels = support::json_object_member(*item, "thinkingLevelMap")) {
                        ai::ThinkingLevelMap mapped;
                        for (const auto& [name, effort] : *levels) {
                            const auto level = ai::parse_model_thinking_level(name);
                            REQUIRE(level);
                            const auto* wire = effort.get_if<std::string>();
                            mapped[*level] = wire ? std::optional<std::string>{*wire} : std::nullopt;
                        }
                        model.thinking_level_map = std::move(mapped);
                    }
                    catalog.push_back(std::move(model));
                }
            }
        }
        REQUIRE(ai::detail::ModelsTestAccess::replace_models(*models, std::move(catalog)));
    }
};

} // namespace cch::tests
