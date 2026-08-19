#pragma once

#include "tui/KillRing.hpp"
#include "tui/UndoStack.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui::detail {

struct BufferCursor {
    std::size_t line{0};
    /// Grapheme column index within line, never a UTF-8 byte offset.
    std::size_t column{0};

    bool operator==(const BufferCursor&) const = default;
};

struct BufferSegment {
    std::string text{};
    std::optional<std::size_t> paste_id{std::nullopt};

    bool operator==(const BufferSegment&) const = default;
};

using BufferLine = std::vector<BufferSegment>;
using BufferDocument = std::vector<BufferLine>;

struct TextBufferOptions {
    bool multiline{true};
    bool enable_paste_markers{true};
    std::shared_ptr<KillRing> kill_ring{nullptr};
};

/// Deep text editing engine encapsulating 2D grapheme documents, embedded paste
/// markers, undo/redo history with typing coalescing, Emacs kill ring / yank-pop,
/// and word/character cursor navigation.
class TextBuffer final {
public:
    explicit TextBuffer(TextBufferOptions options = {});

    // Queries
    [[nodiscard]] std::string text() const;
    [[nodiscard]] std::string expanded_text() const;
    [[nodiscard]] const BufferDocument& document() const;
    [[nodiscard]] std::vector<std::string> lines() const;
    [[nodiscard]] std::vector<std::string> line_strings() const;
    [[nodiscard]] BufferCursor cursor() const;
    void set_cursor(BufferCursor cursor);
    [[nodiscard]] std::size_t line_count() const;
    [[nodiscard]] std::size_t cursor_byte_offset() const;
    [[nodiscard]] std::string line_prefix_before_cursor() const;
    [[nodiscard]] bool empty() const;
    [[nodiscard]] const std::map<std::size_t, std::string>& pastes() const;
    [[nodiscard]] std::size_t paste_counter() const;
    [[nodiscard]] std::shared_ptr<KillRing> kill_ring() const;

    // Content Mutations
    void set_text(std::string text, bool clear_undo_stack = false);
    void insert_character(std::string_view grapheme);
    void insert_text(std::string text, bool record_undo = true);
    void insert_paste(std::string text);
    void insert_newline();

    // Deletions
    void backspace();
    void forward_delete();
    void delete_word_backward();
    void delete_word_forward();
    void kill_to_line_start();
    void kill_to_line_end();

    // Kill Ring & Undo
    void yank();
    void yank_pop();
    void undo();
    void push_undo();
    void clear_undo();

    // Cursor Navigation
    void move_left();
    void move_right();
    void move_up();
    void move_down();
    void move_word_backward();
    void move_word_forward();
    void move_to_line_start();
    void move_to_line_end();
    void move_to_start();
    void move_to_end();
    void jump_to(std::string_view target, bool forward);

    // Advanced Editing Surgery
    void erase_range(BufferCursor start, BufferCursor end);
    void insert_segments(BufferDocument inserted);
    void apply_completion_edit(
        std::size_t line_index,
        std::size_t start_segment,
        std::size_t after_begin,
        std::string_view inserted_middle_text,
        std::string_view result_prefix);

private:
    struct Snapshot {
        BufferDocument document;
        BufferCursor cursor;
        std::map<std::size_t, std::string> pastes;
        std::size_t paste_counter{0};
    };

    struct KillEntry {
        BufferDocument document;
        std::map<std::size_t, std::string> pastes;
    };

    enum class LastAction {
        None,
        TypeWord,
        Kill,
        Yank,
    };

    void clamp_cursor();
    [[nodiscard]] Snapshot create_snapshot() const;
    [[nodiscard]] KillEntry create_kill_entry(BufferDocument killed) const;
    [[nodiscard]] BufferDocument materialize_kill(const KillEntry& entry);
    void forget_paste(std::size_t id);
    [[nodiscard]] BufferLine remove_until(std::size_t target, bool forward);
    [[nodiscard]] BufferDocument segment_lines(std::string_view text);

    TextBufferOptions options_;
    BufferDocument document_{BufferLine{}};
    BufferCursor cursor_{.line = 0, .column = 0};
    std::map<std::size_t, std::string> pastes_{};
    std::size_t paste_counter_{0};
    UndoStack<Snapshot> undo_{};
    std::shared_ptr<KillRing> kill_ring_;
    std::vector<KillEntry> doc_kill_ring_{};
    std::optional<std::size_t> last_yank_ring_index_{std::nullopt};
    std::optional<std::pair<BufferCursor, BufferCursor>> last_yank_range_{std::nullopt};
    LastAction last_action_{LastAction::None};
};

} // namespace cch::tui::detail
