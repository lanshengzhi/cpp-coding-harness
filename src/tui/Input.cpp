#include <cch/tui/Input.hpp>

#include <cch/tui/Utils.hpp>

#include "tui/InteractionUtils.hpp"
#include "tui/TextBuffer.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui {
namespace {

// Behavioral baseline: pi v1.0.4 packages/tui/src/components/input.ts
// (horizontal-scroll windowing with the cursor-at-end column reservation,
// bracketed-paste normalization, kill ring with accumulate/prepend ordering,
// undo with typing coalescing, ICU word navigation, and the submit/escape
// sink routing). Pasted control characters are dropped per this repository's
// decoded-event hygiene, matching Editor::paste.

/// Dispatch order (pi input.ts): cancel, undo, submit, deletions, kill ring,
/// cursor movement, then printable characters. The first bound action in
/// table order wins, so table order IS the precedence.
constexpr std::array<std::string_view, 17> kInputActions = {
        "tui.select.cancel",
        "tui.editor.undo",
        "tui.input.submit",
        "tui.editor.deleteCharBackward",
        "tui.editor.deleteCharForward",
        "tui.editor.deleteWordBackward",
        "tui.editor.deleteWordForward",
        "tui.editor.deleteToLineStart",
        "tui.editor.deleteToLineEnd",
        "tui.editor.yank",
        "tui.editor.yankPop",
        "tui.editor.cursorLeft",
        "tui.editor.cursorRight",
        "tui.editor.cursorLineStart",
        "tui.editor.cursorLineEnd",
        "tui.editor.cursorWordLeft",
        "tui.editor.cursorWordRight",
};

} // namespace

struct Input::Impl {
    InputOptions options;
    InputSubmitSink on_submit;
    InputEscapeSink on_escape;

    detail::TextBuffer buffer{detail::TextBufferOptions{.multiline = false, .enable_paste_markers = false}};
    bool focused{false};
    std::size_t layout_width{0};
    std::optional<support::Error> callback_error;

    [[nodiscard]] std::string value() const {
        return buffer.text();
    }

    [[nodiscard]] std::string prefix() const {
        return buffer.line_prefix_before_cursor();
    }

    void report_callback_failure(std::string message) {
        callback_error = support::make_error(
            support::ErrorCode::Unknown,
            std::move(message),
            "the interaction callback threw an exception");
    }

    void fire_submit() {
        if (!on_submit) return;
        if (auto result = on_submit(value()); !result) {
            // An explicit sink failure is a bounded callback diagnostic;
            // it is recorded with its own message and never vetoes input.
            callback_error = std::move(result.error());
        }
    }

    void fire_escape() {
        if (!on_escape) return;
        if (auto result = on_escape(); !result) {
            callback_error = std::move(result.error());
        }
    }

    void handle_paste(std::string_view text) {
        // The single-line target applies pi's `handlePaste` normalization
        // through the shared insertion path: CR/CRLF/LF are dropped, tabs
        // expand to four spaces, control bytes (including `CSI u` control
        // replies) resolve or drop, and a pasted path gains its separating
        // space. It stays a single undo step.
        buffer.insert_paste(std::string(text));
    }

    struct VisibleWindow {
        std::string text;
        std::size_t before_width{0};
        /// Byte offset of the cursor within `text` (pi's `cursorDisplay`).
        std::size_t cursor_offset{0};
    };

    /// The value window shown at `width` and the visible width of the text
    /// before the cursor within it (pi's render windowing; caller guarantees
    /// `width >= prompt_w` so the prompt's columns are available).
    [[nodiscard]] support::Expected<VisibleWindow> visible_window(std::size_t width) const {
        const auto prompt_w = visible_width(options.prompt);
        const auto available = width > prompt_w ? width - prompt_w : 0;
        const auto text = value();
        const auto total = visible_width(text);
        if (total < available) {
            return VisibleWindow{
                .text = text,
                .before_width = visible_width(prefix()),
                .cursor_offset = prefix().size(),
            };
        }
        // Reserve one column for the cursor when it sits at the end.
        const auto grapheme_count = buffer.document().empty() ? 0 : buffer.document().front().size();
        const auto scroll_width = buffer.cursor().column == grapheme_count ? (available > 0 ? available - 1 : 0) : available;
        if (scroll_width == 0) return VisibleWindow{};
        const auto cursor_col = visible_width(prefix());
        const auto half = scroll_width / 2;
        std::size_t start_col = 0;
        if (cursor_col < half) {
            start_col = 0;
        } else if (cursor_col > total - half) {
            start_col = total > scroll_width ? total - scroll_width : 0;
        } else {
            start_col = cursor_col > half ? cursor_col - half : 0;
        }
        auto visible = slice_by_column(text, start_col, scroll_width, true);
        if (!visible) return std::unexpected(visible.error());
        auto before = slice_by_column(
            text,
            start_col,
            cursor_col > start_col ? cursor_col - start_col : 0,
            true);
        if (!before) return std::unexpected(before.error());
        return VisibleWindow{
            .text = std::move(*visible),
            .before_width = visible_width(*before),
            .cursor_offset = before->size(),
        };
    }
};

Input::Input(InputOptions options, InputSubmitSink on_submit, InputEscapeSink on_escape)
    : impl_(std::make_unique<Impl>()) {
    if (options.keybinding_context) {
        options.keybindings = options.keybinding_context->snapshot();
    } else if (!options.keybindings) {
        options.keybindings = default_tui_keybindings();
    }
    impl_->options = std::move(options);
    impl_->on_submit = std::move(on_submit);
    impl_->on_escape = std::move(on_escape);
}

Input::Input(Input&&) noexcept = default;
Input& Input::operator=(Input&&) noexcept = default;
Input::~Input() = default;

std::string Input::value() const {
    return impl_->value();
}

void Input::set_value(std::string value) {
    const auto prior_col = impl_->buffer.cursor().column;
    impl_->buffer.set_text(std::move(value));
    const auto max_col = impl_->buffer.document().empty() ? 0 : impl_->buffer.document().front().size();
    impl_->buffer.set_cursor(detail::BufferCursor{.line = 0, .column = std::min(prior_col, max_col)});
}

void Input::move_cursor_to_end() { impl_->buffer.move_to_line_end(); }

void Input::set_keybindings(std::shared_ptr<const KeybindingRegistry> keybindings) {
    if (keybindings) {
        impl_->options.keybindings = std::move(keybindings);
        impl_->options.keybinding_context.reset();
    }
}

void Input::set_keybinding_context(std::shared_ptr<KeybindingsContext> context) {
    if (context) {
        impl_->options.keybindings = context->snapshot();
        impl_->options.keybinding_context = std::move(context);
    }
}

support::Expected<RenderResult> Input::render(std::size_t width) {
    if (impl_->callback_error) return std::unexpected(*impl_->callback_error);
    if (width == 0) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Input requires a positive visible width"));
    }
    impl_->layout_width = width;
    const auto& prompt = impl_->options.prompt;
    const auto prompt_w = visible_width(prompt);
    if (width <= prompt_w) {
        auto truncated = truncate_text(prompt, width, "");
        if (!truncated) return std::unexpected(truncated.error());
        return RenderResult{.lines = {std::move(*truncated)}};
    }
    const auto available = width - prompt_w;

    if (impl_->value().empty() && impl_->options.placeholder && !impl_->options.placeholder->empty()) {
        auto placeholder_str = truncate_text(*impl_->options.placeholder, available, "");
        if (!placeholder_str) return std::unexpected(placeholder_str.error());
        const auto graphemes = detail::split_graphemes(*placeholder_str);
        const std::string at_cursor = graphemes.empty() ? std::string(" ") : graphemes.front();
        const std::string after_cursor = placeholder_str->substr(at_cursor.size());

        const auto styled_at_cursor =
                impl_->options.placeholder_style ? impl_->options.placeholder_style(at_cursor) : at_cursor;
        const auto styled_after =
                impl_->options.placeholder_style ? impl_->options.placeholder_style(after_cursor) : after_cursor;

        const std::string cursor_char = "\x1b[7m" + styled_at_cursor + "\x1b[27m";
        const std::string text_with_cursor = cursor_char + styled_after;
        const auto visual_length = visible_width(text_with_cursor);
        const auto padding = available > visual_length ? available - visual_length : 0;
        std::string line = prompt + text_with_cursor + std::string(padding, ' ');
        return RenderResult{.lines = {std::move(line)}};
    }

    auto window = impl_->visible_window(width);
    if (!window) return std::unexpected(window.error());

    // Fake cursor (pi input.ts render): reverse video on the grapheme at the
    // cursor, or a highlighted space at end of line. ESC[27m (reverse off)
    // restores normal rendering for the rest of the line without resetting any
    // caller-applied style.
    const auto before = window->text.substr(0, window->cursor_offset);
    const auto after = window->text.substr(window->cursor_offset);
    const auto graphemes = detail::split_graphemes(after);
    const auto at_cursor = graphemes.empty() ? std::string(" ") : graphemes.front();
    const auto after_cursor = graphemes.empty() ? std::string{} : after.substr(at_cursor.size());

    const std::string cursor_char = "\x1b[7m" + at_cursor + "\x1b[27m";
    const std::string text_with_cursor = before + cursor_char + after_cursor;
    const auto visual_length = visible_width(text_with_cursor);
    const auto padding = available > visual_length ? available - visual_length : 0;
    std::string line = prompt + text_with_cursor + std::string(padding, ' ');
    return RenderResult{.lines = {std::move(line)}};
}

void Input::invalidate() {}

InputAdmissionOutcome Input::handle_input(const InputEventVariant& input) {
    if (const auto* paste = std::get_if<PasteEvent>(&input)) {
        impl_->handle_paste(paste->text);
        return InputAdmissionOutcome::Consumed;
    }
    const auto* event = std::get_if<KeyEvent>(&input);
    if (!carries_press_behavior(event)) {
        return InputAdmissionOutcome::Unhandled;
    }
    const auto keybindings = impl_->options.keybinding_context ? impl_->options.keybinding_context->snapshot()
                                                               : impl_->options.keybindings;
    const auto action = keybindings->first_match(*event, kInputActions);
    if (action == "tui.select.cancel") {
        impl_->fire_escape();
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.undo") {
        impl_->buffer.undo();
        return InputAdmissionOutcome::Consumed;
    }
    const bool is_raw_lf = (event->raw_sequence == "\n") || (event->text == "\n");
    if (action == "tui.input.submit" || is_raw_lf) {
        impl_->fire_submit();
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.deleteCharBackward") {
        impl_->buffer.backspace();
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.deleteCharForward") {
        impl_->buffer.forward_delete();
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.deleteWordBackward") {
        impl_->buffer.delete_word_backward();
        if (impl_->buffer.error()) impl_->callback_error = *impl_->buffer.error();
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.deleteWordForward") {
        impl_->buffer.delete_word_forward();
        if (impl_->buffer.error()) impl_->callback_error = *impl_->buffer.error();
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.deleteToLineStart") {
        impl_->buffer.kill_to_line_start();
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.deleteToLineEnd") {
        impl_->buffer.kill_to_line_end();
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.yank") {
        impl_->buffer.yank();
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.yankPop") {
        impl_->buffer.yank_pop();
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.cursorLeft") {
        impl_->buffer.move_left();
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.cursorRight") {
        impl_->buffer.move_right();
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.cursorLineStart") {
        impl_->buffer.move_to_line_start();
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.cursorLineEnd") {
        impl_->buffer.move_to_line_end();
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.cursorWordLeft") {
        impl_->buffer.move_word_backward();
        if (impl_->buffer.error()) impl_->callback_error = *impl_->buffer.error();
        return InputAdmissionOutcome::Consumed;
    }
    if (action == "tui.editor.cursorWordRight") {
        impl_->buffer.move_word_forward();
        if (impl_->buffer.error()) impl_->callback_error = *impl_->buffer.error();
        return InputAdmissionOutcome::Consumed;
    }
    if (const auto text = printable_text(*event)) {
        impl_->buffer.insert_character(*text);
        return InputAdmissionOutcome::Consumed;
    }
    return InputAdmissionOutcome::Unhandled;
}

void Input::set_focused(bool focused) {
    impl_->focused = focused;
}

bool Input::focused() const {
    return impl_->focused;
}

std::optional<CursorPosition> Input::cursor_location() const {
    if (!impl_->focused || impl_->layout_width == 0) return std::nullopt;
    const auto width = impl_->layout_width;
    const auto prompt_w = visible_width(impl_->options.prompt);
    if (width <= prompt_w) return CursorPosition{.column = 0, .row = 0};
    if (impl_->value().empty() && impl_->options.placeholder && !impl_->options.placeholder->empty()) {
        return CursorPosition{
                .column = std::min(prompt_w, width - 1),
                .row = 0,
        };
    }
    const auto window = impl_->visible_window(width);
    if (!window) return std::nullopt;
    return CursorPosition{
            .column = std::min(prompt_w + window->before_width, width - 1),
            .row = 0,
    };
}

} // namespace cch::tui
