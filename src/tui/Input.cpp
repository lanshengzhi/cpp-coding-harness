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

// Behavioral baseline: pi 83114817 packages/tui/src/components/input.ts
// (horizontal-scroll windowing with the cursor-at-end column reservation,
// bracketed-paste cleaning, kill ring with accumulate/prepend ordering,
// undo with typing coalescing, word navigation, and the submit/escape sink
// routing). Pasted control characters are dropped per this repository's
// decoded-event hygiene, matching Editor::paste; word navigation uses the
// C++ editor's per-grapheme classification (parity map #2 fork-B).

/// The rendered prompt prefix, as in pi's input.ts.
constexpr std::string_view kPrompt = "> ";

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
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            if (auto result = on_submit(value()); !result) {
                // An explicit sink failure is a bounded callback diagnostic;
                // it is recorded with its own message and never vetoes input.
                callback_error = std::move(result.error());
            }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (...) {
            report_callback_failure("TUI Input submit callback failed");
        }
#endif
    }

    void fire_escape() {
        if (!on_escape) return;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            if (auto result = on_escape(); !result) {
                callback_error = std::move(result.error());
            }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (...) {
            report_callback_failure("TUI Input escape callback failed");
        }
#endif
    }

    void handle_paste(std::string_view text) {
        // Single-line cleaning (pi's handlePaste): CRLF/lone CR/lone LF are
        // removed, tabs expand to four spaces. Remaining C0/C1/DEL control
        // characters are dropped per this repository's decoded-event hygiene
        // (matching Editor::paste).
        std::string cleaned;
        for (std::size_t index = 0; index < text.size();) {
            const auto [codepoint, bytes] = detail::decode_utf8(text, index);
            if (bytes == 0) {
                ++index;
                continue;
            }
            if (codepoint == '\r') {
                index += bytes;
                if (index < text.size() && text[index] == '\n') ++index;
                continue;
            }
            if (codepoint == '\n') {
                index += bytes;
                continue;
            }
            if (codepoint == '\t') {
                cleaned += "    ";
                index += bytes;
                continue;
            }
            if (codepoint < 0x20 || codepoint == 0x7f || (codepoint >= 0x80 && codepoint <= 0x9f)) {
                index += bytes;
                continue;
            }
            cleaned.append(text.substr(index, bytes));
            index += bytes;
        }
        buffer.insert_text(cleaned);
    }

    struct VisibleWindow {
        std::string text;
        std::size_t before_width{0};
        /// Byte offset of the cursor within `text` (pi's `cursorDisplay`).
        std::size_t cursor_offset{0};
    };

    /// The value window shown at `width` and the visible width of the text
    /// before the cursor within it (pi's render windowing; caller guarantees
    /// `width >= 2` so the prompt's two columns are available).
    [[nodiscard]] support::Expected<VisibleWindow> visible_window(std::size_t width) const {
        const auto available = width - 2;
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
    if (!options.keybindings) options.keybindings = default_tui_keybindings();
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

support::Expected<RenderResult> Input::render(std::size_t width) {
    if (impl_->callback_error) return std::unexpected(*impl_->callback_error);
    if (width == 0) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Input requires a positive visible width"));
    }
    impl_->layout_width = width;
    if (width < 2) {
        // No room for the prompt; pi returns the bare prompt in this case,
        // truncated here to the width bound.
        return RenderResult{.lines = {">"}};
    }
    auto window = impl_->visible_window(width);
    if (!window) return std::unexpected(window.error());

    // Fake cursor (pi input.ts render): reverse video on the grapheme at the
    // cursor, or a highlighted space at end of line. ESC[27m (reverse off)
    // restores normal rendering for the rest of the line without resetting any
    // caller-applied style.
    const auto before = window->text.substr(0, window->cursor_offset);
    const auto after = window->text.substr(window->cursor_offset);
    std::string body;
    if (after.empty()) {
        body = before + "\x1b[7m \x1b[27m";
        // Optional placeholder: dimmed text after the empty-value cursor,
        // truncated to the columns left after the prompt and the cursor cell
        // (ESC[22m restores normal intensity without resetting caller styles).
        if (impl_->value().empty() && impl_->options.placeholder && !impl_->options.placeholder->empty() &&
                width >= 3) {
            const auto available = width - 3;
            if (available > 0) {
                auto placeholder = truncate_text(*impl_->options.placeholder, available, "");
                if (!placeholder) return std::unexpected(placeholder.error());
                body += "\x1b[2m" + *placeholder + "\x1b[22m";
            }
        }
    } else {
        const auto graphemes = detail::split_graphemes(after);
        const auto& at_cursor = graphemes.front();
        body = before + "\x1b[7m" + at_cursor + "\x1b[27m" + after.substr(at_cursor.size());
    }

    std::string line = std::string(kPrompt) + body;
    const auto line_width = 2 + visible_width(body);
    if (line_width < width) line.append(width - line_width, ' ');
    return RenderResult{.lines = {std::move(line)}};
}

void Input::invalidate() {}

void Input::handle_input(const InputEventVariant& input) {
    if (const auto* paste = std::get_if<PasteEvent>(&input)) {
        impl_->handle_paste(paste->text);
        return;
    }
    const auto* event = std::get_if<KeyEvent>(&input);
    if (event == nullptr || event->type == KeyEventType::Release) return;
    const auto action = impl_->options.keybindings->first_match(*event, kInputActions);
    if (action == "tui.select.cancel") {
        impl_->fire_escape();
        return;
    }
    if (action == "tui.editor.undo") {
        impl_->buffer.undo();
        return;
    }
    if (action == "tui.input.submit") {
        impl_->fire_submit();
        return;
    }
    if (action == "tui.editor.deleteCharBackward") {
        impl_->buffer.backspace();
        return;
    }
    if (action == "tui.editor.deleteCharForward") {
        impl_->buffer.forward_delete();
        return;
    }
    if (action == "tui.editor.deleteWordBackward") {
        impl_->buffer.delete_word_backward();
        return;
    }
    if (action == "tui.editor.deleteWordForward") {
        impl_->buffer.delete_word_forward();
        return;
    }
    if (action == "tui.editor.deleteToLineStart") {
        impl_->buffer.kill_to_line_start();
        return;
    }
    if (action == "tui.editor.deleteToLineEnd") {
        impl_->buffer.kill_to_line_end();
        return;
    }
    if (action == "tui.editor.yank") {
        impl_->buffer.yank();
        return;
    }
    if (action == "tui.editor.yankPop") {
        impl_->buffer.yank_pop();
        return;
    }
    if (action == "tui.editor.cursorLeft") {
        impl_->buffer.move_left();
        return;
    }
    if (action == "tui.editor.cursorRight") {
        impl_->buffer.move_right();
        return;
    }
    if (action == "tui.editor.cursorLineStart") {
        impl_->buffer.move_to_line_start();
        return;
    }
    if (action == "tui.editor.cursorLineEnd") {
        impl_->buffer.move_to_line_end();
        return;
    }
    if (action == "tui.editor.cursorWordLeft") {
        impl_->buffer.move_word_backward();
        return;
    }
    if (action == "tui.editor.cursorWordRight") {
        impl_->buffer.move_word_forward();
        return;
    }
    if (detail::is_printable(*event)) impl_->buffer.insert_character(detail::printable_text(*event));
}

bool Input::accepts_key_releases() const {
    return false;
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
    if (width < 2) return CursorPosition{.column = 0, .row = 0};
    const auto window = impl_->visible_window(width);
    if (!window) return std::nullopt;
    return CursorPosition{
        .column = std::min(2 + window->before_width, width - 1),
        .row = 0,
    };
}

} // namespace cch::tui
