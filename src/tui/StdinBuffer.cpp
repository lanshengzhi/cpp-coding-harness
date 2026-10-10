#include <cch/tui/StdinBuffer.hpp>

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>

namespace cch::tui {
namespace {

constexpr char kEsc = '\x1b';
constexpr std::string_view kPasteStart = "\x1b[200~";
constexpr std::string_view kPasteEnd = "\x1b[201~";

enum class SequenceStatus { Complete, Incomplete, NotEscape };

[[nodiscard]] std::optional<unsigned int> parse_unmodified_kitty_printable_codepoint(std::string_view sequence) {
    if (!sequence.starts_with("\x1b[") || !sequence.ends_with('u')) return std::nullopt;
    const auto body = sequence.substr(2, sequence.size() - 3);
    const auto semicolon = body.find(';');
    const auto key_part = semicolon == std::string_view::npos ? body : body.substr(0, semicolon);
    if (key_part.empty() || !std::ranges::all_of(key_part, [](unsigned char ch) { return std::isdigit(ch); })) {
        return std::nullopt;
    }
    unsigned int codepoint = 0;
    for (const auto ch : key_part)
        codepoint = codepoint * 10 + static_cast<unsigned int>(ch - '0');
    return codepoint >= 32 ? std::optional<unsigned int>{codepoint} : std::nullopt;
}

[[nodiscard]] SequenceStatus is_complete_csi_sequence(std::string_view data) {
    if (!data.starts_with("\x1b[")) return SequenceStatus::Complete;
    if (data.size() < 3) return SequenceStatus::Incomplete;

    const auto payload = data.substr(2);
    if (payload.starts_with("M")) {
        return data.size() >= 6 ? SequenceStatus::Complete : SequenceStatus::Incomplete;
    }

    const auto last = payload.back();
    const auto last_code = static_cast<unsigned char>(last);
    if (last_code >= 0x40 && last_code <= 0x7e) {
        if (payload.starts_with("<")) {
            const auto before_final = payload.substr(0, payload.size() - 1);
            const auto parts = before_final.substr(1);
            std::size_t start = 0;
            int part_count = 0;
            while (start <= parts.size()) {
                const auto separator = parts.find(';', start);
                const auto part = parts.substr(start, separator - start);
                if (part.empty() || !std::ranges::all_of(part, [](unsigned char ch) { return std::isdigit(ch); })) {
                    return SequenceStatus::Incomplete;
                }
                ++part_count;
                if (separator == std::string_view::npos) break;
                start = separator + 1;
            }
            if (part_count == 3 && (last == 'M' || last == 'm')) return SequenceStatus::Complete;
            return SequenceStatus::Incomplete;
        }
        return SequenceStatus::Complete;
    }
    return SequenceStatus::Incomplete;
}

[[nodiscard]] SequenceStatus is_complete_osc_sequence(std::string_view data) {
    if (!data.starts_with("\x1b]")) return SequenceStatus::Complete;
    if (data.ends_with("\x07") || data.ends_with("\x1b\\")) return SequenceStatus::Complete;
    return SequenceStatus::Incomplete;
}

[[nodiscard]] SequenceStatus is_complete_string_terminated_sequence(std::string_view data) {
    if (!data.starts_with("\x1bP") && !data.starts_with("\x1b_")) return SequenceStatus::Complete;
    if (data.ends_with("\x1b\\")) return SequenceStatus::Complete;
    return SequenceStatus::Incomplete;
}

[[nodiscard]] SequenceStatus is_complete_sequence(std::string_view data) {
    if (!data.starts_with(kEsc)) return SequenceStatus::NotEscape;
    if (data.size() == 1) return SequenceStatus::Incomplete;

    const auto after_esc = data.substr(1);
    if (after_esc.starts_with("[")) return is_complete_csi_sequence(data);
    if (after_esc.starts_with("]")) return is_complete_osc_sequence(data);
    if (after_esc.starts_with("P") || after_esc.starts_with("_")) {
        return is_complete_string_terminated_sequence(data);
    }
    if (after_esc.starts_with("O"))
        return after_esc.size() >= 2 ? SequenceStatus::Complete : SequenceStatus::Incomplete;
    if (after_esc.size() == 1) return SequenceStatus::Complete;
    return SequenceStatus::Complete;
}

struct ExtractedSequences {
    std::vector<std::string> sequences;
    std::string remainder;
};

[[nodiscard]] std::size_t utf8_sequence_length(unsigned char lead) {
    // Match frozen pi StdinBuffer string semantics: non-ESC units are UTF-16
    // characters in JS; in C++ emit one complete UTF-8 code unit sequence.
    if ((lead & 0x80) == 0) return 1;
    if ((lead & 0xe0) == 0xc0) return 2;
    if ((lead & 0xf0) == 0xe0) return 3;
    if ((lead & 0xf8) == 0xf0) return 4;
    return 1;
}

[[nodiscard]] ExtractedSequences extract_complete_sequences(std::string_view buffer) {
    ExtractedSequences extracted;
    std::size_t position = 0;
    while (position < buffer.size()) {
        const auto remaining = buffer.substr(position);
        if (!remaining.starts_with(kEsc)) {
            const auto length = utf8_sequence_length(static_cast<unsigned char>(remaining.front()));
            if (remaining.size() < length) {
                extracted.remainder = std::string(remaining);
                break;
            }
            extracted.sequences.emplace_back(remaining.substr(0, length));
            position += length;
            continue;
        }

        std::size_t end = 1;
        while (end <= remaining.size()) {
            const auto candidate = remaining.substr(0, end);
            const auto status = is_complete_sequence(candidate);
            if (status == SequenceStatus::Complete) {
                if (candidate == "\x1b\x1b") {
                    const auto next = end < remaining.size() ? remaining[end] : '\0';
                    if (next == '[' || next == ']' || next == 'O' || next == 'P' || next == '_') {
                        extracted.sequences.emplace_back(1, kEsc);
                        ++position;
                        break;
                    }
                }
                extracted.sequences.push_back(std::string(candidate));
                position += end;
                break;
            }
            if (status == SequenceStatus::Incomplete) {
                ++end;
                continue;
            }
            extracted.sequences.push_back(std::string(candidate));
            position += end;
            break;
        }
        if (end > remaining.size()) {
            extracted.remainder = std::string(remaining);
            break;
        }
    }
    return extracted;
}

} // namespace

StdinBuffer::StdinBuffer(StdinBufferOptions options) : options_(options) {}

void StdinBuffer::emit_data_sequence(std::string sequence) {
    if (sequence.size() == 1) {
        const auto raw_codepoint = static_cast<unsigned char>(sequence.front());
        if (pending_kitty_printable_codepoint_.has_value() && raw_codepoint == *pending_kitty_printable_codepoint_) {
            pending_kitty_printable_codepoint_.reset();
            return;
        }
    }
    pending_kitty_printable_codepoint_ = parse_unmodified_kitty_printable_codepoint(sequence);
    if (data_handler_) data_handler_(std::move(sequence));
}

void StdinBuffer::cancel_timeout() { deadline_ = std::chrono::steady_clock::time_point::max(); }

void StdinBuffer::schedule_timeout() {
    if (buffer_.empty()) {
        cancel_timeout();
        return;
    }
    deadline_ = std::chrono::steady_clock::now() + selected_timeout();
}

void StdinBuffer::process(std::string_view input) {
    cancel_timeout();
    if (input.empty() && buffer_.empty()) {
        emit_data_sequence("");
        return;
    }

    buffer_.append(input);

    if (paste_mode_) {
        paste_buffer_.append(buffer_);
        buffer_.clear();
        const auto end_index = paste_buffer_.find(kPasteEnd);
        if (end_index == std::string::npos) return;

        const auto pasted_content = paste_buffer_.substr(0, end_index);
        const auto remaining = paste_buffer_.substr(end_index + kPasteEnd.size());
        paste_mode_ = false;
        paste_buffer_.clear();
        pending_kitty_printable_codepoint_.reset();
        if (paste_handler_) paste_handler_(pasted_content);
        if (!remaining.empty()) process(remaining);
        return;
    }

    const auto start_index = buffer_.find(kPasteStart);
    if (start_index != std::string::npos) {
        if (start_index > 0) {
            const auto before_paste = extract_complete_sequences(buffer_.substr(0, start_index));
            for (const auto& sequence : before_paste.sequences)
                emit_data_sequence(sequence);
        }
        pending_kitty_printable_codepoint_.reset();
        buffer_ = buffer_.substr(start_index + kPasteStart.size());
        paste_mode_ = true;
        paste_buffer_ = std::move(buffer_);
        buffer_.clear();

        const auto end_index = paste_buffer_.find(kPasteEnd);
        if (end_index == std::string::npos) return;

        const auto pasted_content = paste_buffer_.substr(0, end_index);
        const auto remaining = paste_buffer_.substr(end_index + kPasteEnd.size());
        paste_mode_ = false;
        paste_buffer_.clear();
        pending_kitty_printable_codepoint_.reset();
        if (paste_handler_) paste_handler_(pasted_content);
        if (!remaining.empty()) process(remaining);
        return;
    }

    const auto extracted = extract_complete_sequences(buffer_);
    buffer_ = extracted.remainder;
    for (const auto& sequence : extracted.sequences)
        emit_data_sequence(sequence);
    schedule_timeout();
}

std::vector<std::string> StdinBuffer::flush() {
    cancel_timeout();
    if (buffer_.empty()) return {};
    std::vector<std::string> flushed{std::move(buffer_)};
    buffer_.clear();
    pending_kitty_printable_codepoint_.reset();
    for (const auto& sequence : flushed)
        emit_data_sequence(sequence);
    return flushed;
}

void StdinBuffer::clear() {
    cancel_timeout();
    buffer_.clear();
    paste_mode_ = false;
    paste_buffer_.clear();
    pending_kitty_printable_codepoint_.reset();
}

} // namespace cch::tui
