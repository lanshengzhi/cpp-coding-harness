#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Style.hpp>

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

struct AutocompleteItem {
    std::string value;
    std::string label;
    std::string description;

    bool operator==(const AutocompleteItem&) const = default;
};

struct AutocompleteRequest {
    std::vector<std::string> lines;
    EditorCursor cursor;
    bool force{false};
};

struct AutocompleteSuggestions {
    std::vector<AutocompleteItem> items;
    /// Exact text immediately before the cursor that accepting an item replaces.
    std::string prefix;
};

/// A caller-owned semantic suggestion source. The Editor does not know which
/// product concepts (commands, files, or otherwise) produced its items.
using AutocompleteProvider = std::move_only_function<std::optional<AutocompleteSuggestions>(
    const AutocompleteRequest&)>;
using EditorChangeSink = std::move_only_function<void(std::string)>;
using EditorSubmitSink = std::move_only_function<void(std::string)>;

struct EditorOptions {
    bool disable_submit{false};
    std::size_t max_visible_lines{5};
    std::shared_ptr<const KeybindingRegistry> keybindings{};
};

struct EditorTheme {
    TextStyleHook text{};
};

/// A reusable multiline Unicode editor controlled through semantic input.
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
    void set_theme(EditorTheme theme);
    void set_autocomplete_provider(AutocompleteProvider provider);
    [[nodiscard]] bool autocomplete_open() const;
    [[nodiscard]] std::vector<AutocompleteItem> autocomplete_items() const;

    [[nodiscard]] util::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override;
    void handle_input(const InputEventVariant& input) override;
    [[nodiscard]] bool accepts_key_releases() const override;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    [[nodiscard]] std::optional<CursorPosition> cursor_location() const override;
    void set_available_height(std::size_t rows) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::tui
