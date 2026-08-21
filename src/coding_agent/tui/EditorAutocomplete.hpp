#pragma once

// Editor autocomplete construction for the Native TUI (pi
// `createBaseAutocompleteProvider`): the combined slash-command + file
// provider over the built-in slash commands, the loaded prompt templates,
// and `/skill:` commands, plus the executor-bound debounce timer the editor
// requests completions through. Extraction #506 from the pre-split
// monolith.
//
// Repository-private `cch_coding_agent` implementation header: not part of
// an Owner Interface, not installed, never exported.

#include "coding_agent/tui/ModelFlowController.hpp"

#include <cch/coding_agent/PromptTemplate.hpp>
#include <cch/coding_agent/Skill.hpp>
#include <cch/tui/Autocomplete.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cch::coding_agent::tui {

/// Build the editor autocomplete command list: the 17 Supported built-in
/// slash commands (pi `BUILTIN_SLASH_COMMANDS` subset) as plain items, the
/// loaded prompt templates (scope-prefixed descriptions), and `/skill:`
/// commands while the `enableSkillCommands` setting is enabled — plus the
/// `model` command as a `SlashCommand` whose argument completion resolves
/// pi's `model-search` text over the current candidate snapshot (scoped
/// models when the session carries a scope, else the availability snapshot).
/// The Deferred slashes (`/export` `/import` `/share` `/changelog`
/// `/clone`), `/debug`, and the easter eggs are absent.
[[nodiscard]] std::vector<std::variant<cch::tui::SlashCommand, cch::tui::AutocompleteItem>>
command_autocomplete_commands(
    std::span<const PromptTemplate> prompt_templates,
    std::span<const Skill> skills,
    std::shared_ptr<const ModelCompletionSnapshot> model_completion,
    bool include_skill_commands);

/// Resolve an executable on PATH (pi's `ensureTool`); nullopt when absent so
/// `@`/`#` completion degrades gracefully to empty file suggestions.
[[nodiscard]] std::optional<std::filesystem::path> find_executable_on_path(
    std::string_view name);

/// pi `createBaseAutocompleteProvider`: the combined provider over the
/// effective commands, prompt templates, and (while the
/// `enableSkillCommands` setting is enabled) `skill:` commands, rooted at
/// the session (or boot) workspace for file completion.
[[nodiscard]] std::unique_ptr<cch::tui::AutocompleteProvider>
build_editor_autocomplete_provider(
    std::span<const PromptTemplate> prompt_templates,
    std::span<const Skill> skills,
    std::shared_ptr<const ModelCompletionSnapshot> model_completion,
    bool include_skill_commands,
    const std::filesystem::path& workspace);

/// Executor-bound one-shot debounce timer for the editor's autocomplete
/// requests. All timer state is confined to the executor thread via posts;
/// the shared state keeps the wait handler safe after the editor is gone.
class AsioAutocompleteDebounceTimer final : public cch::tui::AutocompleteDebounceTimer {
public:
    explicit AsioAutocompleteDebounceTimer(boost::asio::any_io_executor executor);
    AsioAutocompleteDebounceTimer(AsioAutocompleteDebounceTimer&&) = delete;
    AsioAutocompleteDebounceTimer& operator=(AsioAutocompleteDebounceTimer&&) = delete;
    ~AsioAutocompleteDebounceTimer() override = default;
    AsioAutocompleteDebounceTimer(const AsioAutocompleteDebounceTimer&) = delete;
    AsioAutocompleteDebounceTimer& operator=(const AsioAutocompleteDebounceTimer&) = delete;

    void start(std::chrono::milliseconds delay,
               std::move_only_function<support::ExpectedVoid()> on_fire) override;
    void cancel() override;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace cch::coding_agent::tui
