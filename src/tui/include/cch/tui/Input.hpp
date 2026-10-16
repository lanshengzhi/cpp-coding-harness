#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Mouse.hpp>

#include <cch/support/Error.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace cch::tui {

/// Called with the current value when the user submits (pi `Input.onSubmit`).
/// Must return promptly and must not re-enter the component. A reported
/// failure is a bounded callback diagnostic; it never vetoes input handling.
using InputSubmitSink = std::move_only_function<support::ExpectedVoid(std::string)>;
/// Called when the user cancels (pi `Input.onEscape`). A reported failure is
/// a bounded callback diagnostic; it never vetoes input handling.
using InputEscapeSink = std::move_only_function<support::ExpectedVoid()>;

/// Custom placeholder styling function (pi `placeholderStyle`). Move-only like
/// the toolkit style hooks (`TextStyleHook`): `InputOptions` is always built
/// as a prvalue and moved into `Input`, never copied.
using PlaceholderStyle = std::move_only_function<std::string(std::string_view)>;

struct InputOptions {
    /// Keybinding registry for action matching; defaults to the built-in
    /// `tui.*` table when null.
    std::shared_ptr<const KeybindingRegistry> keybindings{};
    std::shared_ptr<KeybindingsContext> keybinding_context{};
    /// Prompt prefix rendered before the input (pi `options.prompt ?? "> "`).
    std::string prompt{"> "};
    /// Optional placeholder text displayed while the value is empty (pi `options.placeholder`).
    std::optional<std::string> placeholder{std::nullopt};
    /// Optional styling applied to placeholder characters (pi `options.placeholderStyle`).
    PlaceholderStyle placeholder_style{};
};

/// Single-line text input with horizontal scrolling (pi `components/input.ts`).
/// `render` always returns exactly one line.
///
/// Behaves like pi's Input: bracketed-paste insertion at the cursor, Emacs
/// kill ring with yank/yank-pop, undo with typing coalescing, word
/// navigation, submit/escape routed through the injected sinks, and a left
/// press on the rendered row placing the cursor. The
/// rendered window scrolls around the cursor; the cursor itself is presented
/// through the focus lifecycle (`cursor_location`), following the editor's
/// convention in this repository's decoded-event fork.
class Input final : public Component, public InputHandler, public Focusable, public MouseHandler {
public:
    explicit Input(
        InputOptions options = {},
        InputSubmitSink on_submit = {},
        InputEscapeSink on_escape = {});
    Input(Input&&) noexcept;
    Input& operator=(Input&&) noexcept;
    ~Input() override;

    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    /// Current single-line value (pi `getValue`).
    [[nodiscard]] std::string value() const;
    /// Replace the value, clamping the cursor to the new length (pi `setValue`).
    void set_value(std::string value);
    /// Move the cursor to the end without consulting user keybindings or
    /// changing the value.
    void move_cursor_to_end();
    void set_keybindings(std::shared_ptr<const KeybindingRegistry> keybindings);
    void set_keybinding_context(std::shared_ptr<KeybindingsContext> context);

    [[nodiscard]] support::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override;
    InputAdmissionOutcome handle_input(const InputEventVariant& input) override;
    /// A left press on the single rendered row places the cursor (pi
    /// `Input.handleMouse`). Only press positions the cursor; drag and release
    /// stay unhandled so the renderer's screen-level selection owns them.
    [[nodiscard]] std::optional<MouseEventResult> handle_mouse(const MouseEvent& event) override;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    [[nodiscard]] std::optional<CursorPosition> cursor_location() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::tui
