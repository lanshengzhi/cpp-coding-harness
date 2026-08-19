#include "TextBuffer.hpp"

#include "tui/UnicodeWidth.hpp"
#include "tui/WordNavigation.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui::detail {
namespace {

[[nodiscard]] bool is_whitespace_grapheme(std::string_view segment) {
    if (segment.empty()) return false;
    if (segment == " " || segment == "\t" || segment == "\n" || segment == "\r") return true;
    const auto [codepoint, bytes] = decode_utf8(segment, 0);
    if (bytes == 0) return false;
    switch (codepoint) {
        case 0x00a0:
        case 0x1680:
        case 0x2000:
        case 0x2001:
        case 0x2002:
        case 0x2003:
        case 0x2004:
        case 0x2005:
        case 0x2006:
        case 0x2007:
        case 0x2008:
        case 0x2009:
        case 0x200a:
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

[[nodiscard]] std::string normalize_newlines(std::string text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\r') {
            if (index + 1 < text.size() && text[index + 1] == '\n') {
                ++index;
            }
            result.push_back('\n');
            continue;
        }
        result.push_back(text[index]);
    }
    return result;
}

[[nodiscard]] std::string sanitize_input(std::string text, bool multiline) {
    text = normalize_newlines(std::move(text));
    std::string filtered;
    filtered.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        const auto [codepoint, bytes] = decode_utf8(text, index);
        if (bytes == 0) break;
        if (codepoint == '\n') {
            if (multiline) {
                filtered.push_back('\n');
            } else {
                filtered.push_back(' ');
            }
        } else if (codepoint == '\t' || (codepoint >= 0x20 && codepoint != 0x7f)) {
            filtered.append(text.substr(index, bytes));
        }
        index += bytes;
    }
    return filtered;
}

[[nodiscard]] std::string marker_for(std::size_t id, std::string_view text) {
    const auto lines = static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
    if (lines > 10) {
        return std::format("[paste #{} +{} lines]", id, lines);
    }
    return std::format("[paste #{} {} chars]", id, text.size());
}

[[nodiscard]] std::string line_text(const BufferLine& line) {
    std::string result;
    for (const auto& segment : line) {
        result += segment.text;
    }
    return result;
}

} // namespace

TextBuffer::TextBuffer(TextBufferOptions options)
    : options_(std::move(options)),
      kill_ring_(options_.kill_ring ? options_.kill_ring : std::make_shared<KillRing>()) {}

std::string TextBuffer::text() const {
    std::string result;
    for (std::size_t index = 0; index < document_.size(); ++index) {
        if (index > 0) result += '\n';
        result += line_text(document_[index]);
    }
    return result;
}

std::string TextBuffer::expanded_text() const {
    std::string result;
    for (std::size_t line_index = 0; line_index < document_.size(); ++line_index) {
        if (line_index > 0) result += '\n';
        for (const auto& segment : document_[line_index]) {
            if (segment.paste_id) {
                const auto found = pastes_.find(*segment.paste_id);
                result += found == pastes_.end() ? segment.text : found->second;
            } else {
                result += segment.text;
            }
        }
    }
    return result;
}

const BufferDocument& TextBuffer::document() const {
    return document_;
}

std::vector<std::string> TextBuffer::lines() const {
    return line_strings();
}

std::vector<std::string> TextBuffer::line_strings() const {
    std::vector<std::string> result;
    result.reserve(document_.size());
    for (const auto& line : document_) {
        result.push_back(line_text(line));
    }
    return result;
}

BufferCursor TextBuffer::cursor() const {
    return cursor_;
}

void TextBuffer::set_cursor(BufferCursor cursor) {
    cursor_ = cursor;
    clamp_cursor();
    last_yank_ring_index_.reset();
    last_yank_range_.reset();
    last_action_ = LastAction::None;
}

std::size_t TextBuffer::line_count() const {
    return document_.size();
}

std::size_t TextBuffer::cursor_byte_offset() const {
    if (cursor_.line >= document_.size()) return 0;
    const auto& line = document_[cursor_.line];
    std::size_t bytes = 0;
    for (std::size_t index = 0; index < std::min(cursor_.column, line.size()); ++index) {
        bytes += line[index].text.size();
    }
    return bytes;
}

std::string TextBuffer::line_prefix_before_cursor() const {
    if (cursor_.line >= document_.size()) return {};
    return line_text(document_[cursor_.line]).substr(0, cursor_byte_offset());
}

bool TextBuffer::empty() const {
    return document_.empty() || (document_.size() == 1 && document_.front().empty());
}

const std::map<std::size_t, std::string>& TextBuffer::pastes() const {
    return pastes_;
}

std::size_t TextBuffer::paste_counter() const {
    return paste_counter_;
}

std::shared_ptr<KillRing> TextBuffer::kill_ring() const {
    return kill_ring_;
}

void TextBuffer::clamp_cursor() {
    if (document_.empty()) document_.emplace_back();
    cursor_.line = std::min(cursor_.line, document_.size() - 1);
    cursor_.column = std::min(cursor_.column, document_[cursor_.line].size());
}

TextBuffer::Snapshot TextBuffer::create_snapshot() const {
    return {
        .document = document_,
        .cursor = cursor_,
        .pastes = pastes_,
        .paste_counter = paste_counter_,
    };
}

TextBuffer::KillEntry TextBuffer::create_kill_entry(BufferDocument killed) const {
    KillEntry entry{.document = std::move(killed), .pastes = {}};
    for (const auto& line : entry.document) {
        for (const auto& segment : line) {
            if (segment.paste_id) {
                const auto found = pastes_.find(*segment.paste_id);
                if (found != pastes_.end()) {
                    entry.pastes.emplace(*segment.paste_id, found->second);
                }
            }
        }
    }
    return entry;
}

BufferDocument TextBuffer::materialize_kill(const KillEntry& entry) {
    auto result = entry.document;
    std::map<std::size_t, std::size_t> remapped_ids;
    for (auto& line : result) {
        for (auto& segment : line) {
            if (!segment.paste_id) continue;
            const auto source_id = *segment.paste_id;
            const auto existing = remapped_ids.find(source_id);
            if (existing != remapped_ids.end()) {
                segment.paste_id = existing->second;
                segment.text = marker_for(existing->second, entry.pastes.at(source_id));
                continue;
            }
            const auto target_id = ++paste_counter_;
            const auto found = entry.pastes.find(source_id);
            if (found != entry.pastes.end()) {
                pastes_[target_id] = found->second;
                remapped_ids.emplace(source_id, target_id);
                segment.paste_id = target_id;
                segment.text = marker_for(target_id, found->second);
            }
        }
    }
    return result;
}

void TextBuffer::forget_paste(std::size_t id) {
    pastes_.erase(id);
    for (auto& line : document_) {
        for (auto& segment : line) {
            if (segment.paste_id && *segment.paste_id > id) {
                --*segment.paste_id;
                const auto marker = segment.text;
                const auto number_start = marker.find('#') + 1;
                const auto number_end = marker.find(' ', number_start);
                segment.text = marker.substr(0, number_start) + std::to_string(*segment.paste_id) +
                    marker.substr(number_end);
            }
        }
    }
    std::map<std::size_t, std::string> renumbered;
    for (auto& [paste_id, content] : pastes_) {
        renumbered.emplace(paste_id > id ? paste_id - 1 : paste_id, std::move(content));
    }
    pastes_ = std::move(renumbered);
    if (paste_counter_ > 0) --paste_counter_;
}

BufferLine TextBuffer::remove_until(std::size_t target, bool forward) {
    auto& line = document_[cursor_.line];
    const auto start = forward ? cursor_.column : target;
    const auto end = forward ? target : cursor_.column;
    BufferLine removed(
        line.begin() + static_cast<std::ptrdiff_t>(start),
        line.begin() + static_cast<std::ptrdiff_t>(end));
    line.erase(
        line.begin() + static_cast<std::ptrdiff_t>(start),
        line.begin() + static_cast<std::ptrdiff_t>(end));
    cursor_.column = start;
    return removed;
}

BufferDocument TextBuffer::segment_lines(std::string_view text) {
    BufferDocument result;
    BufferLine current;
    std::size_t line_start = 0;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        if (index == text.size() || (text[index] == '\n' && options_.multiline)) {
            const auto raw_line = text.substr(line_start, index - line_start);
            for (auto&& unit : split_graphemes(raw_line)) {
                current.push_back({.text = std::move(unit), .paste_id = std::nullopt});
            }
            result.push_back(std::move(current));
            current = BufferLine{};
            line_start = index + 1;
        }
    }
    if (result.empty()) result.emplace_back();
    return result;
}

void TextBuffer::push_undo() {
    undo_.push(create_snapshot());
    last_yank_ring_index_.reset();
    last_yank_range_.reset();
}

void TextBuffer::clear_undo() {
    undo_.clear();
    last_yank_ring_index_.reset();
    last_yank_range_.reset();
    last_action_ = LastAction::None;
}

void TextBuffer::set_text(std::string text, bool clear_undo_stack) {
    text = sanitize_input(std::move(text), options_.multiline);
    if (clear_undo_stack) {
        clear_undo();
    }
    pastes_.clear();
    paste_counter_ = 0;
    document_ = segment_lines(text);
    cursor_ = {.line = document_.size() - 1, .column = document_.back().size()};
    clamp_cursor();
}

void TextBuffer::insert_character(std::string_view grapheme) {
    auto units = split_graphemes(grapheme);
    if (units.empty()) return;
    clamp_cursor();

    const auto has_whitespace = std::any_of(units.begin(), units.end(), [](const auto& unit) {
        return is_whitespace_grapheme(unit);
    });
    if (has_whitespace || last_action_ != LastAction::TypeWord) {
        push_undo();
    }
    last_action_ = LastAction::TypeWord;

    auto& line = document_[cursor_.line];
    for (auto&& unit : units) {
        line.insert(
            line.begin() + static_cast<std::ptrdiff_t>(cursor_.column),
            BufferSegment{.text = std::move(unit), .paste_id = std::nullopt});
        ++cursor_.column;
    }
}

void TextBuffer::insert_text(std::string text, bool record_undo) {
    text = sanitize_input(std::move(text), options_.multiline);
    if (text.empty()) return;
    if (record_undo) push_undo();
    last_action_ = LastAction::None;
    insert_segments(segment_lines(text));
}

void TextBuffer::insert_paste(std::string text) {
    text = sanitize_input(std::move(text), options_.multiline);
    if (text.empty()) return;
    push_undo();
    last_action_ = LastAction::None;

    const auto line_count = static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
    if (options_.multiline && options_.enable_paste_markers && (line_count > 10 || text.size() > 1000)) {
        const auto id = ++paste_counter_;
        pastes_.emplace(id, text);
        const auto marker = marker_for(id, text);
        insert_segments({{{.text = marker, .paste_id = id}}});
        return;
    }
    insert_segments(segment_lines(text));
}

void TextBuffer::insert_newline() {
    if (!options_.multiline) return;
    clamp_cursor();
    push_undo();
    last_action_ = LastAction::None;

    auto& current = document_[cursor_.line];
    BufferLine tail(
        current.begin() + static_cast<std::ptrdiff_t>(cursor_.column),
        current.end());
    current.erase(
        current.begin() + static_cast<std::ptrdiff_t>(cursor_.column),
        current.end());
    document_.insert(
        document_.begin() + static_cast<std::ptrdiff_t>(cursor_.line + 1),
        std::move(tail));
    ++cursor_.line;
    cursor_.column = 0;
}

void TextBuffer::insert_segments(BufferDocument inserted) {
    if (inserted.empty()) return;
    clamp_cursor();
    auto& line = document_[cursor_.line];
    BufferLine after(
        line.begin() + static_cast<std::ptrdiff_t>(cursor_.column),
        line.end());
    line.erase(
        line.begin() + static_cast<std::ptrdiff_t>(cursor_.column),
        line.end());

    line.insert(line.end(), inserted.front().begin(), inserted.front().end());
    if (inserted.size() == 1) {
        cursor_.column = line.size();
        line.insert(line.end(), after.begin(), after.end());
        return;
    }

    document_.insert(
        document_.begin() + static_cast<std::ptrdiff_t>(cursor_.line + 1),
        inserted.begin() + 1,
        inserted.end());
    auto* last_inserted = &document_[cursor_.line + inserted.size() - 1];
    last_inserted->insert(last_inserted->end(), after.begin(), after.end());
    cursor_.line += inserted.size() - 1;
    cursor_.column = last_inserted->size() - after.size();
}

void TextBuffer::backspace() {
    clamp_cursor();
    last_action_ = LastAction::None;
    if (cursor_.column > 0) {
        push_undo();
        auto& line = document_[cursor_.line];
        if (line[cursor_.column - 1].paste_id) {
            forget_paste(*line[cursor_.column - 1].paste_id);
        }
        line.erase(line.begin() + static_cast<std::ptrdiff_t>(cursor_.column - 1));
        --cursor_.column;
    } else if (cursor_.line > 0 && options_.multiline) {
        push_undo();
        const auto prior_size = document_[cursor_.line - 1].size();
        document_[cursor_.line - 1].insert(
            document_[cursor_.line - 1].end(),
            document_[cursor_.line].begin(),
            document_[cursor_.line].end());
        document_.erase(document_.begin() + static_cast<std::ptrdiff_t>(cursor_.line));
        --cursor_.line;
        cursor_.column = prior_size;
    }
}

void TextBuffer::forward_delete() {
    clamp_cursor();
    last_action_ = LastAction::None;
    auto& line = document_[cursor_.line];
    if (cursor_.column < line.size()) {
        push_undo();
        if (line[cursor_.column].paste_id) {
            forget_paste(*line[cursor_.column].paste_id);
        }
        line.erase(line.begin() + static_cast<std::ptrdiff_t>(cursor_.column));
    } else if (cursor_.line + 1 < document_.size() && options_.multiline) {
        push_undo();
        line.insert(
            line.end(),
            document_[cursor_.line + 1].begin(),
            document_[cursor_.line + 1].end());
        document_.erase(document_.begin() + static_cast<std::ptrdiff_t>(cursor_.line + 1));
    }
}

void TextBuffer::delete_word_backward() {
    clamp_cursor();
    if (cursor_.column == 0 && (cursor_.line == 0 || !options_.multiline)) return;

    const auto was_kill = last_action_ == LastAction::Kill;
    const auto original = cursor_;
    move_word_backward();
    const auto target = cursor_;
    if (original == target) return;

    cursor_ = target;
    push_undo();

    std::string deleted_text;
    BufferDocument deleted_doc;
    if (original.line == target.line) {
        auto removed = remove_until(original.column, true);
        for (const auto& s : removed) deleted_text += s.text;
        deleted_doc.push_back(std::move(removed));
    } else {
        erase_range(target, original);
    }

    if (kill_ring_) {
        kill_ring_->push(std::move(deleted_text), /*prepend=*/true, was_kill);
    }
    if (was_kill && !doc_kill_ring_.empty()) {
        auto last = std::move(doc_kill_ring_.front());
        doc_kill_ring_.erase(doc_kill_ring_.begin());
        if (!deleted_doc.empty() && !last.document.empty()) {
            deleted_doc.back().insert(deleted_doc.back().end(), last.document.front().begin(), last.document.front().end());
            deleted_doc.insert(deleted_doc.end(), last.document.begin() + 1, last.document.end());
        }
        doc_kill_ring_.insert(doc_kill_ring_.begin(), create_kill_entry(std::move(deleted_doc)));
    } else {
        doc_kill_ring_.insert(doc_kill_ring_.begin(), create_kill_entry(std::move(deleted_doc)));
    }
    last_action_ = LastAction::Kill;
}

void TextBuffer::delete_word_forward() {
    clamp_cursor();
    auto& line = document_[cursor_.line];
    if (cursor_.column >= line.size() && (cursor_.line + 1 >= document_.size() || !options_.multiline)) return;

    const auto was_kill = last_action_ == LastAction::Kill;
    const auto original = cursor_;
    move_word_forward();
    const auto target = cursor_;
    if (original == target) return;

    cursor_ = original;
    push_undo();

    std::string deleted_text;
    BufferDocument deleted_doc;
    if (original.line == target.line) {
        auto removed = remove_until(target.column, true);
        for (const auto& s : removed) deleted_text += s.text;
        deleted_doc.push_back(std::move(removed));
    } else {
        erase_range(original, target);
    }

    if (kill_ring_) {
        kill_ring_->push(std::move(deleted_text), /*prepend=*/false, was_kill);
    }
    if (was_kill && !doc_kill_ring_.empty()) {
        auto last = std::move(doc_kill_ring_.front());
        doc_kill_ring_.erase(doc_kill_ring_.begin());
        last.document.back().insert(last.document.back().end(), deleted_doc.front().begin(), deleted_doc.front().end());
        last.document.insert(last.document.end(), deleted_doc.begin() + 1, deleted_doc.end());
        doc_kill_ring_.insert(doc_kill_ring_.begin(), std::move(last));
    } else {
        doc_kill_ring_.insert(doc_kill_ring_.begin(), create_kill_entry(std::move(deleted_doc)));
    }
    last_action_ = LastAction::Kill;
}

void TextBuffer::kill_to_line_start() {
    clamp_cursor();
    const auto was_kill = last_action_ == LastAction::Kill;
    const auto target = std::size_t{0};
    if (target != cursor_.column) {
        push_undo();
        auto removed = remove_until(target, false);
        std::string removed_str;
        for (const auto& s : removed) removed_str += s.text;
        if (kill_ring_) kill_ring_->push(std::move(removed_str), /*prepend=*/true, was_kill);
        doc_kill_ring_.insert(doc_kill_ring_.begin(), create_kill_entry(BufferDocument{std::move(removed)}));
        last_action_ = LastAction::Kill;
    } else if (cursor_.line > 0 && options_.multiline) {
        push_undo();
        if (kill_ring_) kill_ring_->push("\n", /*prepend=*/true, was_kill);
        doc_kill_ring_.insert(doc_kill_ring_.begin(), create_kill_entry(BufferDocument{BufferLine{}, BufferLine{}}));
        const auto prior_size = document_[cursor_.line - 1].size();
        document_[cursor_.line - 1].insert(
            document_[cursor_.line - 1].end(),
            document_[cursor_.line].begin(),
            document_[cursor_.line].end());
        document_.erase(document_.begin() + static_cast<std::ptrdiff_t>(cursor_.line));
        --cursor_.line;
        cursor_.column = prior_size;
        last_action_ = LastAction::Kill;
    }
}

void TextBuffer::kill_to_line_end() {
    clamp_cursor();
    const auto was_kill = last_action_ == LastAction::Kill;
    const auto target = document_[cursor_.line].size();
    if (target != cursor_.column) {
        push_undo();
        auto removed = remove_until(target, true);
        std::string removed_str;
        for (const auto& s : removed) removed_str += s.text;
        if (kill_ring_) kill_ring_->push(std::move(removed_str), /*prepend=*/false, was_kill);
        doc_kill_ring_.insert(doc_kill_ring_.begin(), create_kill_entry(BufferDocument{std::move(removed)}));
        last_action_ = LastAction::Kill;
    } else if (cursor_.line + 1 < document_.size() && options_.multiline) {
        push_undo();
        if (kill_ring_) kill_ring_->push("\n", /*prepend=*/false, was_kill);
        doc_kill_ring_.insert(doc_kill_ring_.begin(), create_kill_entry(BufferDocument{BufferLine{}, BufferLine{}}));
        document_[cursor_.line].insert(
            document_[cursor_.line].end(),
            document_[cursor_.line + 1].begin(),
            document_[cursor_.line + 1].end());
        document_.erase(document_.begin() + static_cast<std::ptrdiff_t>(cursor_.line + 1));
        last_action_ = LastAction::Kill;
    }
}

void TextBuffer::yank() {
    if (!doc_kill_ring_.empty()) {
        push_undo();
        const auto start = cursor_;
        insert_segments(materialize_kill(doc_kill_ring_.front()));
        last_yank_ring_index_ = 0;
        last_yank_range_ = std::pair{start, cursor_};
        last_action_ = LastAction::Yank;
    } else if (kill_ring_ && kill_ring_->peek() != nullptr) {
        push_undo();
        const auto start = cursor_;
        insert_text(*kill_ring_->peek(), /*record_undo=*/false);
        last_yank_ring_index_ = 0;
        last_yank_range_ = std::pair{start, cursor_};
        last_action_ = LastAction::Yank;
    }
}

void TextBuffer::yank_pop() {
    if (last_action_ != LastAction::Yank || !last_yank_ring_index_ || !last_yank_range_) return;
    if (doc_kill_ring_.size() < 2 && (!kill_ring_ || kill_ring_->length() < 2)) return;

    const auto previous = *last_yank_range_;
    const auto prior_ring_index = *last_yank_ring_index_;
    if (!options_.multiline && previous.first.line != previous.second.line) return;

    push_undo();
    cursor_ = previous.first;
    erase_range(previous.first, previous.second);

    if (!doc_kill_ring_.empty()) {
        const auto next = (prior_ring_index + 1) % doc_kill_ring_.size();
        insert_segments(materialize_kill(doc_kill_ring_[next]));
        last_yank_ring_index_ = next;
        last_yank_range_ = std::pair{previous.first, cursor_};
        last_action_ = LastAction::Yank;
    } else if (kill_ring_) {
        kill_ring_->rotate();
        const auto* text = kill_ring_->peek();
        if (text != nullptr) {
            insert_text(*text, /*record_undo=*/false);
            last_yank_range_ = std::pair{previous.first, cursor_};
            last_action_ = LastAction::Yank;
        }
    }
}

void TextBuffer::undo() {
    auto popped = undo_.pop();
    if (!popped) return;
    document_ = std::move(popped->document);
    cursor_ = popped->cursor;
    pastes_ = std::move(popped->pastes);
    paste_counter_ = popped->paste_counter;
    last_yank_ring_index_.reset();
    last_yank_range_.reset();
    last_action_ = LastAction::None;
    clamp_cursor();
}

void TextBuffer::move_left() {
    clamp_cursor();
    if (cursor_.column > 0) {
        --cursor_.column;
    } else if (cursor_.line > 0 && options_.multiline) {
        --cursor_.line;
        cursor_.column = document_[cursor_.line].size();
    }
    last_yank_ring_index_.reset();
    last_action_ = LastAction::None;
}

void TextBuffer::move_right() {
    clamp_cursor();
    if (cursor_.column < document_[cursor_.line].size()) {
        ++cursor_.column;
    } else if (cursor_.line + 1 < document_.size() && options_.multiline) {
        ++cursor_.line;
        cursor_.column = 0;
    }
    last_yank_ring_index_.reset();
    last_action_ = LastAction::None;
}

void TextBuffer::move_up() {
    if (!options_.multiline) return;
    clamp_cursor();
    if (cursor_.line > 0) {
        --cursor_.line;
        cursor_.column = std::min(cursor_.column, document_[cursor_.line].size());
    }
    last_yank_ring_index_.reset();
    last_action_ = LastAction::None;
}

void TextBuffer::move_down() {
    if (!options_.multiline) return;
    clamp_cursor();
    if (cursor_.line + 1 < document_.size()) {
        ++cursor_.line;
        cursor_.column = std::min(cursor_.column, document_[cursor_.line].size());
    }
    last_yank_ring_index_.reset();
    last_action_ = LastAction::None;
}

void TextBuffer::move_word_backward() {
    clamp_cursor();
    if (options_.multiline) {
        while (cursor_.line > 0 || cursor_.column > 0) {
            if (cursor_.column == 0) {
                --cursor_.line;
                cursor_.column = document_[cursor_.line].size();
                continue;
            }
            if (is_word_segment(document_[cursor_.line][cursor_.column - 1].text)) break;
            --cursor_.column;
        }
        while (cursor_.column > 0 && is_word_segment(document_[cursor_.line][cursor_.column - 1].text)) {
            --cursor_.column;
        }
    } else {
        std::vector<std::string> graphemes;
        for (const auto& s : document_.front()) graphemes.push_back(s.text);
        cursor_.column = find_word_backward(graphemes, cursor_.column);
    }
    last_yank_ring_index_.reset();
    last_action_ = LastAction::None;
}

void TextBuffer::move_word_forward() {
    clamp_cursor();
    if (options_.multiline) {
        while (cursor_.line + 1 < document_.size() || cursor_.column < document_[cursor_.line].size()) {
            if (cursor_.column == document_[cursor_.line].size()) {
                ++cursor_.line;
                cursor_.column = 0;
                continue;
            }
            if (is_word_segment(document_[cursor_.line][cursor_.column].text)) break;
            ++cursor_.column;
        }
        while (cursor_.column < document_[cursor_.line].size() &&
               is_word_segment(document_[cursor_.line][cursor_.column].text)) {
            ++cursor_.column;
        }
    } else {
        std::vector<std::string> graphemes;
        for (const auto& s : document_.front()) graphemes.push_back(s.text);
        cursor_.column = find_word_forward(graphemes, cursor_.column);
    }
    last_yank_ring_index_.reset();
    last_action_ = LastAction::None;
}

void TextBuffer::move_to_line_start() {
    cursor_.column = 0;
    last_yank_ring_index_.reset();
    last_action_ = LastAction::None;
}

void TextBuffer::move_to_line_end() {
    clamp_cursor();
    cursor_.column = document_[cursor_.line].size();
    last_yank_ring_index_.reset();
    last_action_ = LastAction::None;
}

void TextBuffer::move_to_start() {
    cursor_ = {.line = 0, .column = 0};
    last_yank_ring_index_.reset();
    last_action_ = LastAction::None;
}

void TextBuffer::move_to_end() {
    cursor_.line = document_.empty() ? 0 : document_.size() - 1;
    cursor_.column = document_.empty() ? 0 : document_.back().size();
    last_yank_ring_index_.reset();
    last_action_ = LastAction::None;
}

void TextBuffer::jump_to(std::string_view target, bool forward) {
    clamp_cursor();
    if (forward) {
        for (std::size_t line_index = cursor_.line; line_index < document_.size(); ++line_index) {
            const auto start = line_index == cursor_.line ? cursor_.column + 1 : std::size_t{0};
            for (std::size_t index = start; index < document_[line_index].size(); ++index) {
                if (document_[line_index][index].text == target) {
                    cursor_ = {.line = line_index, .column = index};
                    last_yank_ring_index_.reset();
                    last_action_ = LastAction::None;
                    return;
                }
            }
            if (!options_.multiline) break;
        }
    } else {
        for (std::size_t line_index = cursor_.line + 1; line_index-- > 0;) {
            const auto start = line_index == cursor_.line ? cursor_.column : document_[line_index].size();
            for (std::size_t index = start; index-- > 0;) {
                if (document_[line_index][index].text == target) {
                    cursor_ = {.line = line_index, .column = index};
                    last_yank_ring_index_.reset();
                    last_action_ = LastAction::None;
                    return;
                }
            }
            if (!options_.multiline || line_index == 0) break;
        }
    }
    last_yank_ring_index_.reset();
    last_action_ = LastAction::None;
}

void TextBuffer::erase_range(BufferCursor start, BufferCursor end) {
    if (start.line > end.line || (start.line == end.line && start.column > end.column)) {
        std::swap(start, end);
    }
    if (start.line >= document_.size() || end.line >= document_.size()) return;

    if (start.line == end.line) {
        auto& line = document_[start.line];
        const auto clamped_start = std::min(start.column, line.size());
        const auto clamped_end = std::min(end.column, line.size());
        for (std::size_t col = clamped_start; col < clamped_end; ++col) {
            if (line[col].paste_id) forget_paste(*line[col].paste_id);
        }
        line.erase(
            line.begin() + static_cast<std::ptrdiff_t>(clamped_start),
            line.begin() + static_cast<std::ptrdiff_t>(clamped_end));
    } else {
        auto& first = document_[start.line];
        const auto clamped_start = std::min(start.column, first.size());
        for (std::size_t col = clamped_start; col < first.size(); ++col) {
            if (first[col].paste_id) forget_paste(*first[col].paste_id);
        }
        first.erase(first.begin() + static_cast<std::ptrdiff_t>(clamped_start), first.end());

        for (std::size_t line_idx = start.line + 1; line_idx < end.line; ++line_idx) {
            for (const auto& s : document_[line_idx]) {
                if (s.paste_id) forget_paste(*s.paste_id);
            }
        }

        auto& last = document_[end.line];
        const auto clamped_end = std::min(end.column, last.size());
        for (std::size_t col = 0; col < clamped_end; ++col) {
            if (last[col].paste_id) forget_paste(*last[col].paste_id);
        }
        BufferLine tail(
            last.begin() + static_cast<std::ptrdiff_t>(clamped_end),
            last.end());
        first.insert(first.end(), tail.begin(), tail.end());

        document_.erase(
            document_.begin() + static_cast<std::ptrdiff_t>(start.line + 1),
            document_.begin() + static_cast<std::ptrdiff_t>(end.line + 1));
    }
    cursor_ = start;
    clamp_cursor();
}

void TextBuffer::apply_completion_edit(
    std::size_t line_index,
    std::size_t start_segment,
    std::size_t after_begin,
    std::string_view inserted_middle_text,
    std::string_view result_prefix) {
    if (line_index >= document_.size()) return;
    auto& line = document_[line_index];
    BufferLine rebuilt;
    rebuilt.reserve(start_segment + (line.size() - after_begin) + 1);
    rebuilt.insert(
        rebuilt.end(),
        line.begin(),
        line.begin() + static_cast<std::ptrdiff_t>(start_segment));
    auto middle_segments = segment_lines(inserted_middle_text).front();
    rebuilt.insert(
        rebuilt.end(),
        std::make_move_iterator(middle_segments.begin()),
        std::make_move_iterator(middle_segments.end()));
    rebuilt.insert(
        rebuilt.end(),
        line.begin() + static_cast<std::ptrdiff_t>(after_begin),
        line.end());
    line = std::move(rebuilt);

    cursor_.line = line_index;
    cursor_.column = segment_lines(result_prefix).front().size();
    clamp_cursor();
    last_yank_ring_index_.reset();
    last_yank_range_.reset();
    last_action_ = LastAction::None;
}

} // namespace cch::tui::detail
