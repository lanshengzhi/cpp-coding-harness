#pragma once

#include <cch/tui/Autocomplete.hpp>
#include <cch/tui/Editor.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cch::tui::detail {

/// Value facts describing one autocomplete intent. Trigger classification and
/// debounce selection stay with Editor because they inspect its TextBuffer.
struct EditorCompletionIntent {
    bool force{false};
    bool explicit_tab{false};
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

struct EditorCompletionRefresh {
    EditorCompletionIntent intent{};
    std::chrono::milliseconds debounce{};
};

struct EditorCompletionWake {};
struct EditorCompletionCancel {};

enum class EditorCompletionMenuAction {
    MoveUp,
    MoveDown,
    Accept,
    Confirm,
};

struct EditorCompletionMenuInteraction {
    EditorCompletionMenuAction action{EditorCompletionMenuAction::MoveUp};
};

using EditorCompletionInteraction = std::
        variant<EditorCompletionRefresh, EditorCompletionWake, EditorCompletionCancel, EditorCompletionMenuInteraction>;

/// The menu presentation returned with every completion effect. Editor keeps
/// this passive projection for its public menu queries and input routing; the
/// session remains the authority for menu decisions.
struct EditorCompletionMenuPresentation {
    bool open{false};
    bool forced{false};
    std::vector<AutocompleteItem> items{};
    std::string prefix{};
    std::size_t selected_index{0};
};

/// Text surgery has to re-enter Editor so TextBuffer remains the sole owner of
/// document mutation. The session owns provider execution and returns this
/// value; Editor applies it through its serialized entry point.
struct EditorCompletionApplication {
    AutocompleteApplyResult result{};
    std::string prefix{};
    bool notify{true};
};

/// One passive decision produced by a semantic completion interaction.
struct EditorCompletionEffect {
    EditorCompletionMenuPresentation menu{};
    std::optional<EditorCompletionApplication> application{};
    bool submit{false};
};

/// The provider replacement result. Trigger characters belong to Editor's
/// TextBuffer trigger classification; the menu projection belongs to Session.
struct EditorCompletionProviderSetup {
    std::vector<std::string> trigger_characters{};
    EditorCompletionMenuPresentation menu{};
};

using EditorCompletionWakeSink = std::move_only_function<support::ExpectedVoid()>;

/// Private owner of one Editor autocomplete lifecycle. Its provider, debounce
/// timer, request identity, cancellation, pending delivery, and menu state are
/// hidden behind a PImpl. Callback closures use weak control state and never
/// capture Editor or an Editor::Impl strongly.
class EditorCompletionSession final {
public:
    EditorCompletionSession(std::unique_ptr<AutocompleteDebounceTimer> debounce_timer,
            EditorCompletionWakeSink on_wake,
            EditorRenderRequestSink render_request);

    EditorCompletionSession(EditorCompletionSession&&) noexcept;
    EditorCompletionSession& operator=(EditorCompletionSession&&) noexcept;
    ~EditorCompletionSession();
    EditorCompletionSession(const EditorCompletionSession&) = delete;
    EditorCompletionSession& operator=(const EditorCompletionSession&) = delete;

    /// Replaces the provider after cancelling the previous lifecycle and
    /// returns the trigger characters and cleared menu projection.
    [[nodiscard]] EditorCompletionProviderSetup set_provider(std::unique_ptr<AutocompleteProvider> provider);

    /// Handles one semantic interaction. All calls enter through Editor's
    /// serialized path; asynchronous callbacks only record work and request a
    /// wake or render notification.
    [[nodiscard]] support::Expected<EditorCompletionEffect> handle(
            EditorCompletionInteraction interaction, EditorCompletionView view);

    /// Closes the callback control before the owner releases the PImpl.
    void close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::tui::detail
