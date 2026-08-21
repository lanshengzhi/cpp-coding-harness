#include "EditorAutocomplete.hpp"

#include "coding_agent/prompt/BuiltinSlashCommands.hpp"
#include "coding_agent/tui/ModelSearch.hpp"

#include <cch/tui/Fuzzy.hpp>

#include <boost/asio/post.hpp>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>

#include <unistd.h>

namespace cch::coding_agent::tui {
namespace {

/// pi `prefixAutocompleteDescription` subset (`core/source-info.ts` + the
/// interactive-mode `getAutocompleteSourceTag`): the scope-prefixed
/// description for discovered prompt templates and skills. The loader subset
/// produces only `source` "auto"/"cli" (no npm/git sources), so the tag is
/// the scope letter `[u]`/`[p]`/`[t]` alone.
[[nodiscard]] std::string prefix_autocomplete_description(
    const std::string& description,
    SourceScope scope) {
    const char tag = scope == SourceScope::User ? 'u' : scope == SourceScope::Project ? 'p' : 't';
    return std::format("[{}]{}", tag, description.empty() ? "" : " " + description);
}

} // namespace

std::vector<std::variant<cch::tui::SlashCommand, cch::tui::AutocompleteItem>>
command_autocomplete_commands(
    std::span<const PromptTemplate> prompt_templates,
    std::span<const Skill> skills,
    std::shared_ptr<const ModelCompletionSnapshot> model_completion,
    bool include_skill_commands) {
    std::vector<std::variant<cch::tui::SlashCommand, cch::tui::AutocompleteItem>> items;
    std::set<std::string, std::less<>> names;
    for (const auto& command : prompt::builtin_slash_commands()) {
        std::string description = std::string{command.description};
        if (!command.argument_hint.empty()) {
            description = description.empty()
                ? std::string{command.argument_hint}
                : std::format("{} — {}", command.argument_hint, description);
        }
        if (command.name == "model") {
            // pi `createBaseAutocompleteProvider`: `/model` argument
            // completion over `getModelSearchText`, value `provider/id`,
            // label `id`, description `provider`. The description stays
            // plain: the combined provider prepends the argument hint.
            cch::tui::SlashCommand slash;
            slash.name = std::string{command.name};
            slash.description = std::string{command.description};
            slash.argument_hint = std::string{command.argument_hint};
            slash.get_argument_completions =
                [model_completion](std::string_view prefix)
                -> std::optional<std::vector<cch::tui::AutocompleteItem>> {
                    if (!model_completion || model_completion->empty()) return std::nullopt;
                    std::vector<ModelSearchItem> items;
                    items.reserve(model_completion->size());
                    for (const auto& candidate : *model_completion) {
                        items.push_back(ModelSearchItem{
                            .id = candidate.id,
                            .provider = candidate.provider,
                            .name = candidate.name.empty()
                                ? std::nullopt
                                : std::optional<std::string>{candidate.name},
                        });
                    }
                    const auto filtered = cch::tui::fuzzy_filter(
                        std::move(items), prefix, get_model_search_text);
                    if (filtered.empty()) return std::nullopt;
                    std::vector<cch::tui::AutocompleteItem> result;
                    result.reserve(filtered.size());
                    for (const auto& item : filtered) {
                        result.push_back(cch::tui::AutocompleteItem{
                            .value = item.provider + "/" + item.id,
                            .label = item.id,
                            .description = item.provider,
                        });
                    }
                    return result;
                };
            items.push_back(std::move(slash));
        } else {
            items.push_back(cch::tui::AutocompleteItem{
                .value = std::string{command.name},
                .label = std::string{command.name},
                .description = std::move(description),
            });
        }
        names.insert(std::string{command.name});
    }
    for (const auto& prompt_template : prompt_templates) {
        if (!names.insert(prompt_template.name).second) continue;
        std::string description = prompt_template.description.value_or("");
        if (prompt_template.argument_hint && !prompt_template.argument_hint->empty()) {
            description = description.empty()
                ? *prompt_template.argument_hint
                : std::format("{} — {}", *prompt_template.argument_hint, description);
        }
        items.push_back(cch::tui::AutocompleteItem{
            .value = prompt_template.name,
            .label = prompt_template.name,
            .description =
                prefix_autocomplete_description(description, prompt_template.sourceInfo.scope),
        });
    }
    // pi `createBaseAutocompleteProvider`: skill commands register only
    // while the `enableSkillCommands` setting is enabled.
    if (include_skill_commands) {
        for (const auto& skill : skills) {
            auto name = "skill:" + skill.name;
            if (!names.insert(name).second) continue;
            items.push_back(cch::tui::AutocompleteItem{
                .value = name,
                .label = name,
                .description =
                    prefix_autocomplete_description(skill.description, skill.sourceInfo.scope),
            });
        }
    }
    std::sort(items.begin(), items.end(), [](const auto& left, const auto& right) {
        const auto label = [](const auto& item) -> std::string_view {
            if (const auto* slash = std::get_if<cch::tui::SlashCommand>(&item)) {
                return slash->name;
            }
            return std::get<cch::tui::AutocompleteItem>(item).label;
        };
        return label(left) < label(right);
    });
    return items;
}

std::optional<std::filesystem::path> find_executable_on_path(std::string_view name) {
    const char* path_env = std::getenv("PATH");
    if (path_env == nullptr) return std::nullopt;
    std::string_view path_view{path_env};
    std::size_t begin = 0;
    for (std::size_t index = 0; index <= path_view.size(); ++index) {
        if (index != path_view.size() && path_view[index] != ':') continue;
        const auto dir = path_view.substr(begin, index - begin);
        begin = index + 1;
        if (dir.empty()) continue;
        const auto candidate = std::filesystem::path{dir} / name;
        std::error_code error;
        const auto status = std::filesystem::status(candidate, error);
        if (error || !std::filesystem::is_regular_file(status)) continue;
        if (::access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return std::nullopt;
}

std::unique_ptr<cch::tui::AutocompleteProvider> build_editor_autocomplete_provider(
    std::span<const PromptTemplate> prompt_templates,
    std::span<const Skill> skills,
    std::shared_ptr<const ModelCompletionSnapshot> model_completion,
    bool include_skill_commands,
    const std::filesystem::path& workspace) {
    return std::make_unique<cch::tui::CombinedAutocompleteProvider>(
        command_autocomplete_commands(
            prompt_templates,
            skills,
            std::move(model_completion),
            include_skill_commands),
        workspace,
        find_executable_on_path("fd"));
}

struct AsioAutocompleteDebounceTimer::State {
    explicit State(boost::asio::any_io_executor executor)
        : executor(executor), timer(executor) {}
    boost::asio::any_io_executor executor;
    boost::asio::steady_timer timer;
    std::size_t generation{0};
    std::move_only_function<support::ExpectedVoid()> active_callback;
};

AsioAutocompleteDebounceTimer::AsioAutocompleteDebounceTimer(
    boost::asio::any_io_executor executor)
    : state_(std::make_shared<State>(std::move(executor))) {}

void AsioAutocompleteDebounceTimer::start(
    std::chrono::milliseconds delay,
    std::move_only_function<support::ExpectedVoid()> on_fire) {
    const auto state = state_;
    boost::asio::post(state->executor, [state, delay, on_fire = std::move(on_fire)]() mutable {
        const auto generation = ++state->generation;
        state->active_callback = std::move(on_fire);
        state->timer.expires_after(delay);
        state->timer.async_wait([state, generation](boost::system::error_code error) {
            if (generation != state->generation) return;
            auto callback = std::move(state->active_callback);
            state->active_callback = nullptr;
            if (!error && callback) (void)callback();
        });
    });
}

void AsioAutocompleteDebounceTimer::cancel() {
    const auto state = state_;
    boost::asio::post(state->executor, [state] {
        ++state->generation;
        state->active_callback = nullptr;
        state->timer.cancel();
    });
}

} // namespace cch::coding_agent::tui
