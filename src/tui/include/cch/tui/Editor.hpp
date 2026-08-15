#pragma once

#include <cch/tui/Autocomplete.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Style.hpp>

#include <cch/support/Error.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tui {

struct EditorCursor {
    std::size_t line{0};
    /// Grapheme offset within line, never a UTF-8 byte offset.
    std::size_t column{0};

    bool operator==(const EditorCursor&) const = default;
};

/// A caller-provided asynchronous suggestion source. The Editor owns the
/// provider and does not know which product concepts (commands, files, or
/// otherwise) produced its items. The editor serializes all requests: a new
/// request cancels the previous one, and stale results are rejected by
/// generation and snapshot checks.
using EditorChangeSink = std::move_only_function<void(std::string)>;
using EditorSubmitSink = std::move_only_function<void(std::string)>;

/// Notification that the editor's presentation changed asynchronously (an
/// autocomplete result arrived). Must return promptly and must not re-enter
/// the editor; may be invoked from any thread.
using EditorRenderRequestSink = std::move_only_function<void()>;

struct EditorOptions {
    std::size_t max_visible_lines{5};
    std::shared_ptr<const KeybindingRegistry> keybindings{};
    /// One-shot timer for autocomplete debounce; without one, debounced
    /// requests run immediately (deterministic tests inject a manual timer).
    std::unique_ptr<AutocompleteDebounceTimer> autocomplete_debounce_timer{};
    /// Fired when an asynchronous autocomplete result lands so the host can
    /// schedule a repaint.
    EditorRenderRequestSink autocomplete_render_request{};
};

struct EditorTheme {
    TextStyleHook text{};
    /// Optional top/bottom border hook (pi `editor-component.ts`
    /// `borderColor`). An empty hook renders no border lines. When set, the
    /// editor reserves two rows for the borders and renders pi's scroll
    /// indicators (`─── ↑ N more` / `─── ↓ N more`) while scrolled.
    TextStyleHook border{};
};

/// A reusable multiline Unicode editor controlled through semantic input.
///
/// Public methods serialize internally and may be called from any thread; the
/// autocomplete result sink and render-request sink may be invoked from any
/// thread. Caller-provided sinks (change/submit/render-request) must not
/// re-enter the editor.
class Editor final : public Component, public InputHandler, public Focusable, public ViewportAware {
public:
    explicit Editor(
        EditorOptions options = {},
        EditorChangeSink on_change = {},
        EditorSubmitSink on_submit = {});
    Editor(Editor&&) noexcept;
    Editor& operator=(Editor&&) noexcept;
    ~Editor() override;

    Editor(const Editor&) = delete;
    Editor& operator=(const Editor&) = delete;

    [[nodiscard]] std::string text() const;
    [[nodiscard]] std::string expanded_text() const;
    [[nodiscard]] std::vector<std::string> lines() const;
    [[nodiscard]] EditorCursor cursor() const;
    void set_text(std::string text);
    void insert_text_at_cursor(std::string text);
    /// Record a submitted prompt for cursor-boundary up/down recall (pi addToHistory).
    void add_to_history(std::string text);
    void set_theme(EditorTheme theme);
    void set_autocomplete_provider(std::unique_ptr<AutocompleteProvider> provider);
    /// Swap the live keybinding table (pi's shared KeybindingsManager reload
    /// shape over the immutable registry, ADR 0035): subsequent input matches
    /// the new registry. Confined to the app layer's `/reload` re-catalog.
    void set_keybindings(std::shared_ptr<const KeybindingRegistry> keybindings);
    [[nodiscard]] bool autocomplete_open() const;
    [[nodiscard]] std::vector<AutocompleteItem> autocomplete_items() const;
    [[nodiscard]] std::size_t autocomplete_selected_index() const;

    [[nodiscard]] support::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override;
    void handle_input(const InputEventVariant& input) override;
    [[nodiscard]] bool accepts_key_releases() const override;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    [[nodiscard]] std::optional<CursorPosition> cursor_location() const override;
    void set_available_height(std::size_t rows) override;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace cch::tui
