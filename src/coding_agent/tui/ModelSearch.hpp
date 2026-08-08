#pragma once

#include <optional>
#include <string>

namespace cch::coding_agent::tui {

/// pi `ModelSearchItem` (`modes/interactive/model-search.ts`).
struct ModelSearchItem {
    std::string id;
    std::string provider;
    std::optional<std::string> name{std::nullopt};
};

/// pi `getModelSearchText`: the scoped-models selector's fuzzy search text.
/// The bare model id leads so scope entries rank by id first.
[[nodiscard]] inline std::string get_model_search_text(const ModelSearchItem& item) {
    const auto name = item.name ? " " + *item.name : "";
    return item.id + " " + item.provider + " " + item.provider + "/" + item.id +
        " " + item.provider + " " + item.id + name;
}

/// pi `getModelSelectorSearchText`: the /model selector's fuzzy search text.
/// Exact provider-prefixed queries rank before proxy-provider ids like
/// openrouter/openai/gpt-5, so the bare model id stays out of the leading
/// position.
[[nodiscard]] inline std::string get_model_selector_search_text(const ModelSearchItem& item) {
    const auto name = item.name ? " " + *item.name : "";
    return item.provider + " " + item.provider + "/" + item.id + " " + item.provider +
        " " + item.id + name;
}

} // namespace cch::coding_agent::tui
