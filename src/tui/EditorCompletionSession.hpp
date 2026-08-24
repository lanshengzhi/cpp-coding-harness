#pragma once

#include <cch/tui/Autocomplete.hpp>
#include <cch/tui/Editor.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::tui::detail {

/// Value facts describing one autocomplete intent. Trigger classification and
/// debounce selection stay with Editor because they inspect its TextBuffer.
struct EditorCompletionIntent {
    bool force{false};
    bool explicit_tab{false};
};

/// A timer callback's identity. The callback carries no Editor reference; the
/// serialized Editor entry point samples the live view after validating this
/// generation.
struct EditorCompletionDue {
    std::size_t generation{0};
    EditorCompletionIntent intent{};
};

/// The live TextBuffer facts needed by the completion session. `text` and
/// `cursor` are the acceptance snapshot; `lines` and byte offsets are the
/// provider request/application inputs.
struct EditorCompletionView {
    std::string text{};
    EditorCursor cursor{};
    std::vector<std::string> lines{};
    std::size_t cursor_line{0};
    std::size_t cursor_column{0};
};

/// A validated due event plus the current Editor view sampled at request start.
struct EditorCompletionStart {
    EditorCompletionDue due{};
    EditorCompletionView view{};
};

/// Text surgery has to re-enter Editor so TextBuffer remains the sole owner of
/// document mutation. The session owns provider execution and returns this
/// value; Editor applies it through its serialized entry point.
struct EditorCompletionApplication {
    AutocompleteApplyResult result{};
    std::string prefix{};
    bool notify{true};
};

using EditorCompletionDueSink = std::move_only_function<support::ExpectedVoid(EditorCompletionDue)>;

/// Private owner of one Editor autocomplete lifecycle. Its provider, debounce
/// timer, request identity, cancellation, pending delivery, and menu state are
/// hidden behind a PImpl. Callback closures use a weak control state and never
/// capture Editor or an Editor::Impl strongly.
class EditorCompletionSession final {
public:
    EditorCompletionSession(std::unique_ptr<AutocompleteDebounceTimer> debounce_timer,
            EditorCompletionDueSink on_due,
            EditorRenderRequestSink render_request);

    EditorCompletionSession(EditorCompletionSession&&) noexcept;
    EditorCompletionSession& operator=(EditorCompletionSession&&) noexcept;
    ~EditorCompletionSession();
    EditorCompletionSession(const EditorCompletionSession&) = delete;
    EditorCompletionSession& operator=(const EditorCompletionSession&) = delete;

    /// Replaces the provider after cancelling the previous lifecycle and
    /// returns its extra trigger characters for Editor's TextBuffer context.
    [[nodiscard]] std::vector<std::string> set_provider(std::unique_ptr<AutocompleteProvider> provider);

    /// Invalidates every queued/late callback and clears menu state.
    void cancel() noexcept;
    /// Closes the callback control before the owner releases the PImpl.
    void close() noexcept;

    [[nodiscard]] bool has_provider() const;
    [[nodiscard]] bool should_trigger_file_completion(const EditorCompletionView& view) const;

    /// Supersedes the current request. A delayed request returns no due value;
    /// an immediate request returns the due value for serialized start.
    [[nodiscard]] std::optional<EditorCompletionDue> request(
            EditorCompletionIntent intent, std::chrono::milliseconds debounce);

    /// Starts a provider request only when the due generation is current.
    [[nodiscard]] support::ExpectedVoid start(EditorCompletionStart start);

    /// Consumes one pending provider delivery. Stale snapshots are benign
    /// drops; a valid normal result updates the retained menu. A unique forced
    /// result returns an application for Editor's TextBuffer surgery.
    [[nodiscard]] std::optional<EditorCompletionApplication> drain(const EditorCompletionView& view);

    [[nodiscard]] bool open() const;
    [[nodiscard]] bool forced() const;
    [[nodiscard]] std::vector<AutocompleteItem> items() const;
    [[nodiscard]] std::size_t selected_index() const;
    [[nodiscard]] bool prefix_starts_with(std::string_view prefix) const;
    void move_selection(bool down);

    /// Applies the selected item through the provider and returns the value
    /// for Editor's TextBuffer surgery. The session is cancelled before the
    /// value is returned, so no late result can reopen the menu.
    [[nodiscard]] std::optional<EditorCompletionApplication> accept(
            const EditorCompletionView& view, bool fallthrough_submit);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::tui::detail
