#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>

#include <cch/support/Error.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace cch::tui {

/// Called with the current value when the user submits (pi `Input.onSubmit`).
/// Must return promptly and must not re-enter the component. A reported
/// failure is a bounded callback diagnostic; it never vetoes input handling.
using InputSubmitSink = std::move_only_function<support::ExpectedVoid(std::string)>;
/// Called when the user cancels (pi `Input.onEscape`). A reported failure is
/// a bounded callback diagnostic; it never vetoes input handling.
using InputEscapeSink = std::move_only_function<support::ExpectedVoid()>;

struct InputOptions {
    /// Keybinding registry for action matching; defaults to the built-in
    /// `tui.*` table when null.
    std::shared_ptr<const KeybindingRegistry> keybindings{};
    /// Optional prompt text rendered dimmed after the cursor while the value
    /// is empty (and truncated to the remaining line width). Never rendered
    /// once the value is non-empty or when no placeholder is supplied.
    std::optional<std::string> placeholder{std::nullopt};
};

/// Single-line text input with horizontal scrolling (pi `components/input.ts`).
/// `render` always returns exactly one line.
///
/// Behaves like pi's Input: bracketed-paste insertion at the cursor, Emacs
/// kill ring with yank/yank-pop, undo with typing coalescing, word
/// navigation, and submit/escape routed through the injected sinks. The
/// rendered window scrolls around the cursor; the cursor itself is presented
/// through the focus lifecycle (`cursor_location`), following the editor's
/// convention in this repository's decoded-event fork.
class Input final : public Component, public InputHandler, public Focusable {
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

    [[nodiscard]] support::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override;
    void handle_input(const InputEventVariant& input) override;
    [[nodiscard]] bool accepts_key_releases() const override;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    [[nodiscard]] std::optional<CursorPosition> cursor_location() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::tui
