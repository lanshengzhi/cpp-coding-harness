#pragma once

#include <cch/support/Error.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cch::tui {

// Behavioral baseline: pi 83114817 packages/tui/src/autocomplete.ts
// (values, the async provider contract, and CombinedAutocompleteProvider
// semantics; package surface boundary per ADR 0035).

/// One suggestion. `description` is empty when pi's optional description is
/// absent.
struct AutocompleteItem {
    std::string value;
    std::string label;
    std::string description{};

    bool operator==(const AutocompleteItem&) const = default;
};

/// A slash command offered to the combined provider (pi `SlashCommand`).
/// The optional argument-completion callback mirrors pi's
/// `getArgumentCompletions(argumentPrefix)`: it returns std::nullopt when no
/// argument completion is available.
struct SlashCommand {
    std::string name{};
    std::string description{};
    std::string argument_hint{};
    std::move_only_function<std::optional<std::vector<AutocompleteItem>>(std::string_view)>
        get_argument_completions{};
};

/// What the current buffer region is matched against (pi
/// `AutocompleteSuggestions.prefix`), plus the matching items.
struct AutocompleteSuggestions {
    std::vector<AutocompleteItem> items;
    std::string prefix;
};

/// One asynchronous suggestion request. `lines` are the unexpanded editor
/// lines and `cursor_line`/`cursor_column` index into them; the cursor column
/// is a UTF-8 byte offset into its line, never a grapheme offset. The
/// provider observes cancellation through `stop_token`.
struct AutocompleteRequest {
    std::vector<std::string> lines;
    std::size_t cursor_line{0};
    std::size_t cursor_column{0};
    bool force{false};
    std::stop_token stop_token{};
};

/// The result of pi's `applyCompletion` text surgery: the full new buffer
/// plus the cursor position (UTF-8 byte offset into `lines[cursor_line]`).
struct AutocompleteApplyResult {
    std::vector<std::string> lines;
    std::size_t cursor_line{0};
    std::size_t cursor_column{0};
};

/// Called exactly once per request with std::nullopt when no suggestions are
/// available. May be invoked synchronously inside `get_suggestions` or later
/// from any thread; the receiver must be safe for cross-thread invocation.
using AutocompleteResultSink = std::move_only_function<support::ExpectedVoid(std::optional<AutocompleteSuggestions>)>;

/// The asynchronous suggestion source (pi `AutocompleteProvider`).
class AutocompleteProvider {
public:
    virtual ~AutocompleteProvider() = default;

    /// Request suggestions for the current buffer/cursor. Must eventually
    /// invoke `sink` exactly once with the suggestions or std::nullopt, even
    /// when the request is superseded or aborted.
    virtual void get_suggestions(const AutocompleteRequest& request, AutocompleteResultSink sink) = 0;

    /// Perform pi's text surgery replacing the completion prefix region with
    /// the selected item's value. Only the line containing the cursor may
    /// change; the returned lines must match the input lines elsewhere.
    [[nodiscard]] virtual AutocompleteApplyResult apply_completion(
        const std::vector<std::string>& lines,
        std::size_t cursor_line,
        std::size_t cursor_column,
        const AutocompleteItem& item,
        std::string_view prefix) = 0;

    /// Whether an explicit (Tab) file completion may run (pi's optional
    /// `shouldTriggerFileCompletion`; the absent case returns true).
    [[nodiscard]] virtual bool should_trigger_file_completion(
        const std::vector<std::string>& lines,
        std::size_t cursor_line,
        std::size_t cursor_column) const = 0;

    /// Extra single-character auto-triggers merged into the editor's default
    /// `@`/`#` set (pi `triggerCharacters`); empty means the defaults.
    [[nodiscard]] virtual std::vector<std::string> trigger_characters() const = 0;
};

/// One-shot debounce timer for the editor's async autocomplete requests.
/// `start` must invoke `on_fire` at most once after the delay and never after
/// `cancel`. `start`/`cancel` may be called concurrently from any thread.
class AutocompleteDebounceTimer {
public:
    virtual ~AutocompleteDebounceTimer() = default;
    virtual void start(std::chrono::milliseconds delay, std::move_only_function<support::ExpectedVoid()> on_fire) = 0;
    virtual void cancel() = 0;
};

/// Slash-command completion plus `@`/`#` fd-backed filesystem completion (pi
/// `CombinedAutocompleteProvider`). `commands` holds SlashCommand or plain
/// AutocompleteItem values; `base_path` roots relative path completion;
/// `fd_path` is the resolved `fd` executable, or std::nullopt to degrade
/// gracefully to empty file suggestions (pi's `fdPath: null` branch).
class CombinedAutocompleteProvider final : public AutocompleteProvider {
public:
    CombinedAutocompleteProvider(
        std::vector<std::variant<SlashCommand, AutocompleteItem>> commands,
        std::filesystem::path base_path,
        std::optional<std::filesystem::path> fd_path = std::nullopt);
    ~CombinedAutocompleteProvider() override;

    CombinedAutocompleteProvider(CombinedAutocompleteProvider&&) noexcept;
    CombinedAutocompleteProvider& operator=(CombinedAutocompleteProvider&&) noexcept;
    CombinedAutocompleteProvider(const CombinedAutocompleteProvider&) = delete;
    CombinedAutocompleteProvider& operator=(const CombinedAutocompleteProvider&) = delete;

    void get_suggestions(const AutocompleteRequest& request, AutocompleteResultSink sink) override;
    [[nodiscard]] AutocompleteApplyResult apply_completion(
        const std::vector<std::string>& lines,
        std::size_t cursor_line,
        std::size_t cursor_column,
        const AutocompleteItem& item,
        std::string_view prefix) override;
    [[nodiscard]] bool should_trigger_file_completion(
        const std::vector<std::string>& lines,
        std::size_t cursor_line,
        std::size_t cursor_column) const override;
    [[nodiscard]] std::vector<std::string> trigger_characters() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::tui
