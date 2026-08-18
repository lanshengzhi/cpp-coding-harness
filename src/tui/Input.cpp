#include <cch/tui/Input.hpp>

#include <cch/tui/Utils.hpp>

#include "tui/InteractionUtils.hpp"
#include "tui/KillRing.hpp"
#include "tui/UndoStack.hpp"
#include "tui/UnicodeWidth.hpp"
#include "tui/WordNavigation.hpp"

#include <cch/support/Error.hpp>
#include <algorithm>
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

/// JS `\s` equivalent for a single grapheme (pi utils.ts isWhitespaceChar).
[[nodiscard]] bool is_whitespace_grapheme(std::string_view grapheme) {
    const auto [codepoint, bytes] = detail::decode_utf8(grapheme, 0);
    if (bytes == 0) return false;
    if (codepoint >= 0x2000 && codepoint <= 0x200a) return true;
    switch (codepoint) {
        case 0x09:
        case 0x0a:
        case 0x0b:
        case 0x0c:
        case 0x0d:
        case 0x20:
        case 0xa0:
        case 0x1680:
        case 0x2028:
        case 0x2029:
        case 0x202f:
        case 0x205f:
        case 0x3000:
        case 0xfeff:
            return true;
        default:
            return false;
    }
}

} // namespace

struct Input::Impl {
    struct Snapshot {
        std::vector<std::string> graphemes;
        std::size_t cursor{0};
    };

    InputOptions options;
    InputSubmitSink on_submit;
    InputEscapeSink on_escape;

    /// The single-line value as graphemes; `cursor` is a grapheme offset,
    /// never a UTF-8 byte offset (matching EditorCursor's convention).
    std::vector<std::string> graphemes;
    std::size_t cursor{0};
    detail::KillRing kill_ring;
    detail::UndoStack<Snapshot> undo;
    enum class LastAction { None, Kill, Yank, TypeWord };
    LastAction last_action{LastAction::None};
    bool focused{false};
    std::size_t layout_width{0};
    std::optional<support::Error> callback_error;

    [[nodiscard]] std::string value() const {
        std::string result;
        for (const auto& grapheme : graphemes) result += grapheme;
        return result;
    }

    [[nodiscard]] std::string prefix() const {
        std::string result;
        const auto end = std::min(cursor, graphemes.size());
        for (std::size_t index = 0; index < end; ++index) result += graphemes[index];
        return result;
    }

    [[nodiscard]] std::string suffix(std::size_t start) const {
        std::string result;
        for (std::size_t index = start; index < graphemes.size(); ++index) result += graphemes[index];
        return result;
    }

    void push_undo() {
        undo.push({.graphemes = graphemes, .cursor = cursor});
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

    void insert_character(std::string_view text) {
        auto units = detail::split_graphemes(text);
        if (units.empty()) return;
        // Undo coalescing: consecutive word characters coalesce into one undo
        // unit; any whitespace breaks the run (pi's insertCharacter).
        const auto has_whitespace = std::any_of(units.begin(), units.end(), [](const auto& unit) {
            return is_whitespace_grapheme(unit);
        });
        if (has_whitespace || last_action != LastAction::TypeWord) push_undo();
        last_action = LastAction::TypeWord;
        graphemes.insert(
            graphemes.begin() + static_cast<std::ptrdiff_t>(cursor),
            units.begin(),
            units.end());
        cursor += units.size();
    }

    void backspace() {
        last_action = LastAction::None;
        if (cursor == 0) return;
        push_undo();
        graphemes.erase(graphemes.begin() + static_cast<std::ptrdiff_t>(cursor - 1));
        --cursor;
    }

    void forward_delete() {
        last_action = LastAction::None;
        if (cursor >= graphemes.size()) return;
        push_undo();
        graphemes.erase(graphemes.begin() + static_cast<std::ptrdiff_t>(cursor));
    }

    void delete_to_line_start() {
        if (cursor == 0) return;
        push_undo();
        kill_ring.push(prefix(), /*prepend=*/true, last_action == LastAction::Kill);
        last_action = LastAction::Kill;
        graphemes.erase(graphemes.begin(), graphemes.begin() + static_cast<std::ptrdiff_t>(cursor));
        cursor = 0;
    }

    void delete_to_line_end() {
        if (cursor >= graphemes.size()) return;
        push_undo();
        kill_ring.push(suffix(cursor), /*prepend=*/false, last_action == LastAction::Kill);
        last_action = LastAction::Kill;
        graphemes.erase(
            graphemes.begin() + static_cast<std::ptrdiff_t>(cursor),
            graphemes.end());
    }

    void delete_word_backward() {
        if (cursor == 0) return;
        // Save the kill state before the cursor movement resets it.
        const auto was_kill = last_action == LastAction::Kill;
        push_undo();
        const auto delete_from = detail::find_word_backward(graphemes, cursor);
        std::string deleted;
        for (std::size_t index = delete_from; index < cursor; ++index) deleted += graphemes[index];
        kill_ring.push(std::move(deleted), /*prepend=*/true, was_kill);
        last_action = LastAction::Kill;
        graphemes.erase(
            graphemes.begin() + static_cast<std::ptrdiff_t>(delete_from),
            graphemes.begin() + static_cast<std::ptrdiff_t>(cursor));
        cursor = delete_from;
    }

    void delete_word_forward() {
        if (cursor >= graphemes.size()) return;
        const auto was_kill = last_action == LastAction::Kill;
        push_undo();
        const auto delete_to = detail::find_word_forward(graphemes, cursor);
        std::string deleted;
        for (std::size_t index = cursor; index < delete_to; ++index) deleted += graphemes[index];
        kill_ring.push(std::move(deleted), /*prepend=*/false, was_kill);
        last_action = LastAction::Kill;
        graphemes.erase(
            graphemes.begin() + static_cast<std::ptrdiff_t>(cursor),
            graphemes.begin() + static_cast<std::ptrdiff_t>(delete_to));
    }

    void yank() {
        const auto* text = kill_ring.peek();
        if (text == nullptr) return;
        push_undo();
        const auto units = detail::split_graphemes(*text);
        graphemes.insert(
            graphemes.begin() + static_cast<std::ptrdiff_t>(cursor),
            units.begin(),
            units.end());
        cursor += units.size();
        last_action = LastAction::Yank;
    }

    void yank_pop() {
        if (last_action != LastAction::Yank || kill_ring.length() <= 1) return;
        const auto* previous = kill_ring.peek();
        if (previous == nullptr) return;
        const auto previous_units = detail::split_graphemes(*previous);
        if (previous_units.size() > cursor) return;
        push_undo();
        // Delete the previously yanked text (still the ring tail before rotation).
        graphemes.erase(
            graphemes.begin() + static_cast<std::ptrdiff_t>(cursor - previous_units.size()),
            graphemes.begin() + static_cast<std::ptrdiff_t>(cursor));
        cursor -= previous_units.size();
        // Rotate and insert the new entry.
        kill_ring.rotate();
        const auto units = detail::split_graphemes(*kill_ring.peek());
        graphemes.insert(
            graphemes.begin() + static_cast<std::ptrdiff_t>(cursor),
            units.begin(),
            units.end());
        cursor += units.size();
        last_action = LastAction::Yank;
    }

    void undo_once() {
        const auto snapshot = undo.pop();
        if (!snapshot) return;
        graphemes = std::move(snapshot->graphemes);
        cursor = snapshot->cursor;
        last_action = LastAction::None;
    }

    void move_left() {
        last_action = LastAction::None;
        if (cursor > 0) --cursor;
    }

    void move_right() {
        last_action = LastAction::None;
        if (cursor < graphemes.size()) ++cursor;
    }

    void move_line_start() {
        last_action = LastAction::None;
        cursor = 0;
    }

    void move_line_end() {
        last_action = LastAction::None;
        cursor = graphemes.size();
    }

    void move_word_backward() {
        if (cursor == 0) return;
        last_action = LastAction::None;
        cursor = detail::find_word_backward(graphemes, cursor);
    }

    void move_word_forward() {
        if (cursor >= graphemes.size()) return;
        last_action = LastAction::None;
        cursor = detail::find_word_forward(graphemes, cursor);
    }

    void handle_paste(std::string_view text) {
        last_action = LastAction::None;
        push_undo();
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
        const auto units = detail::split_graphemes(cleaned);
        graphemes.insert(
            graphemes.begin() + static_cast<std::ptrdiff_t>(cursor),
            units.begin(),
            units.end());
        cursor += units.size();
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
        const auto scroll_width = cursor == graphemes.size() ? (available > 0 ? available - 1 : 0) : available;
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
    impl_->graphemes = detail::split_graphemes(value);
    impl_->cursor = std::min(impl_->cursor, impl_->graphemes.size());
}

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
    const auto matches = [this, event](std::string_view action_id) {
        return impl_->options.keybindings->matches(*event, action_id);
    };

    // Action order follows pi's input.ts: cancel, undo, submit, deletions,
    // kill ring, cursor movement, then printable characters.
    if (matches("tui.select.cancel")) {
        impl_->fire_escape();
        return;
    }
    if (matches("tui.editor.undo")) {
        impl_->undo_once();
        return;
    }
    if (matches("tui.input.submit")) {
        impl_->fire_submit();
        return;
    }
    if (matches("tui.editor.deleteCharBackward")) {
        impl_->backspace();
        return;
    }
    if (matches("tui.editor.deleteCharForward")) {
        impl_->forward_delete();
        return;
    }
    if (matches("tui.editor.deleteWordBackward")) {
        impl_->delete_word_backward();
        return;
    }
    if (matches("tui.editor.deleteWordForward")) {
        impl_->delete_word_forward();
        return;
    }
    if (matches("tui.editor.deleteToLineStart")) {
        impl_->delete_to_line_start();
        return;
    }
    if (matches("tui.editor.deleteToLineEnd")) {
        impl_->delete_to_line_end();
        return;
    }
    if (matches("tui.editor.yank")) {
        impl_->yank();
        return;
    }
    if (matches("tui.editor.yankPop")) {
        impl_->yank_pop();
        return;
    }
    if (matches("tui.editor.cursorLeft")) {
        impl_->move_left();
        return;
    }
    if (matches("tui.editor.cursorRight")) {
        impl_->move_right();
        return;
    }
    if (matches("tui.editor.cursorLineStart")) {
        impl_->move_line_start();
        return;
    }
    if (matches("tui.editor.cursorLineEnd")) {
        impl_->move_line_end();
        return;
    }
    if (matches("tui.editor.cursorWordLeft")) {
        impl_->move_word_backward();
        return;
    }
    if (matches("tui.editor.cursorWordRight")) {
        impl_->move_word_forward();
        return;
    }
    if (detail::is_printable(*event)) impl_->insert_character(detail::printable_text(*event));
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
