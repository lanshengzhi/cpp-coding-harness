#include "UserBashOutputAccumulator.hpp"

#include "coding_agent/BoundedText.hpp"
#include "util/BoundedText.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <random>
#include <system_error>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace cch::coding_agent::runtime {

namespace user_bash_output_detail {

// --- Stage 1: incremental UTF-8 safety -------------------------------------

void Utf8Safety::append(std::string_view raw, std::string& out) {
    buffer_.append(raw);
    std::size_t index = 0;
    while (index < buffer_.size()) {
        const auto lead = static_cast<unsigned char>(buffer_[index]);
        const std::size_t length = sequence_length(lead);
        if (length == 0) {
            out += kUtf8Replacement;
            ++index;
            continue;
        }
        if (index + length > buffer_.size()) {
            if (plausible_prefix(buffer_.substr(index))) {
                break; // hold back; a later chunk may complete it
            }
            out += kUtf8Replacement;
            ++index;
            continue;
        }
        if (!valid_sequence(buffer_, index, length)) {
            out += kUtf8Replacement;
            ++index;
            continue;
        }
        out.append(buffer_, index, length);
        index += length;
    }
    buffer_.erase(0, index);
}

void Utf8Safety::finish(std::string& out) {
    // Leftover bytes can never complete: each becomes U+FFFD, matching
    // bounded_utf8 on a truncated whole buffer.
    for (std::size_t index = 0; index < buffer_.size(); ++index) {
        out += kUtf8Replacement;
    }
    buffer_.clear();
}

std::size_t Utf8Safety::sequence_length(unsigned char lead) {
    if (lead <= 0x7f) return 1;
    if (lead >= 0xc2 && lead <= 0xdf) return 2;
    if (lead >= 0xe0 && lead <= 0xef) return 3;
    if (lead >= 0xf0 && lead <= 0xf4) return 4;
    return 0;
}

bool Utf8Safety::plausible_prefix(std::string_view rest) {
    const std::size_t length = sequence_length(static_cast<unsigned char>(rest.front()));
    if (length == 0 || rest.size() >= length) return false;
    for (std::size_t offset = 1; offset < rest.size(); ++offset) {
        const auto continuation = static_cast<unsigned char>(rest[offset]);
        if (continuation < 0x80 || continuation > 0xbf) return false;
    }
    return true;
}

bool Utf8Safety::valid_sequence(
    std::string_view text,
    std::size_t index,
    std::size_t length) {
    for (std::size_t offset = 1; offset < length; ++offset) {
        const auto continuation = static_cast<unsigned char>(text[index + offset]);
        if (continuation < 0x80 || continuation > 0xbf) return false;
    }
    const auto lead = static_cast<unsigned char>(text[index]);
    if (length == 3) {
        const auto second = static_cast<unsigned char>(text[index + 1]);
        return !((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second >= 0xa0));
    }
    if (length == 4) {
        const auto second = static_cast<unsigned char>(text[index + 1]);
        return !((lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second >= 0x90));
    }
    return true;
}

// --- Stage 2: incremental ANSI stripping ------------------------------------

void AnsiStrip::append(std::string_view text, std::string& out) {
    buffer_.append(text);
    std::size_t index = 0;
    while (index < buffer_.size()) {
        const auto esc = buffer_.find('\x1b', index);
        if (esc == std::string::npos) {
            out.append(buffer_, index, std::string::npos);
            index = buffer_.size();
            break;
        }
        out.append(buffer_, index, esc - index);
        const auto end = sequence_end(esc);
        if (end == std::string::npos) {
            if (buffer_.size() - esc > kMaxEscapeBytes) {
                buffer_.clear(); // drop the over-long unterminated sequence
                return;
            }
            index = esc;
            break; // hold back the partial sequence
        }
        index = end;
    }
    buffer_.erase(0, index);
}

void AnsiStrip::finish(std::string&) {
    // An unterminated trailing escape is stripped, matching the
    // whole-buffer stripper, which consumes it to end of input.
    buffer_.clear();
}

std::size_t AnsiStrip::sequence_end(std::size_t esc) const {
    if (esc + 1 >= buffer_.size()) return std::string::npos;
    if (buffer_[esc + 1] == '[') {
        for (std::size_t index = esc + 2; index < buffer_.size(); ++index) {
            const auto current = static_cast<unsigned char>(buffer_[index]);
            if (current >= 0x40 && current <= 0x7e) return index + 1;
        }
        return std::string::npos;
    }
    if (buffer_[esc + 1] == ']') {
        for (std::size_t index = esc + 2; index < buffer_.size(); ++index) {
            if (buffer_[index] == '\a') return index + 1;
            if (buffer_[index] == '\x1b') {
                if (index + 1 >= buffer_.size()) return std::string::npos;
                if (buffer_[index + 1] == '\\') return index + 2;
            }
        }
        return std::string::npos;
    }
    return esc + 2 <= buffer_.size() ? esc + 2 : std::string::npos;
}

// --- Stage 3: carriage-return normalization ---------------------------------

void CarriageReturnNormalize::append(std::string_view text, std::string& out) {
    std::string_view rest = text;
    if (held_cr_) {
        held_cr_ = false;
        out.push_back('\n');
        if (!rest.empty() && rest.front() == '\n') rest.remove_prefix(1);
    }
    while (!rest.empty()) {
        const char ch = rest.front();
        if (ch == '\r') {
            if (rest.size() == 1) {
                held_cr_ = true;
                break;
            }
            out.push_back('\n');
            const bool crlf = rest[1] == '\n';
            rest.remove_prefix(crlf ? 2 : 1);
            continue;
        }
        const auto value = static_cast<unsigned char>(ch);
        if (value < 0x20 && ch != '\n' && ch != '\t') {
            rest.remove_prefix(1);
            continue;
        }
        out.push_back(ch);
        rest.remove_prefix(1);
    }
}

void CarriageReturnNormalize::finish(std::string& out) {
    if (held_cr_) {
        held_cr_ = false;
        out.push_back('\n');
    }
}

// --- Stage 4: incremental secret redaction -----------------------------------

void RedactEmit::append(std::string_view text, std::string& out) {
    pending_.append(text);
    suppress_continuation();
    pending_ = util::redact_text(std::move(pending_));
    arm_suppression();
    const std::size_t emit = emit_length();
    out.append(pending_, 0, emit);
    pending_.erase(0, emit);
    suppress_from_ = emit < suppress_from_ ? suppress_from_ - emit : 0;
}

void RedactEmit::finish(std::string& out) {
    suppress_continuation();
    pending_ = util::redact_text(std::move(pending_));
    out.append(pending_);
    pending_.clear();
    suppression_ = Suppression::None;
    suppress_from_ = 0;
    authorization_ = false;
}

bool RedactEmit::is_key_character(char ch) {
    const auto value = static_cast<unsigned char>(ch);
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9') || ch == '_' || ch == '-';
}

// Unquoted value termination, mirroring redact_assignments: Authorization
// keys additionally allow whitespace inside the value.
bool RedactEmit::is_value_delimiter(char ch, bool authorization) {
    const auto value = static_cast<unsigned char>(ch);
    return ch == ',' || ch == '}' || ch == '\n' ||
        (!authorization && std::isspace(value));
}

bool RedactEmit::ends_with_marker(std::string_view text) {
    return text.size() >= util::kRedactionMarker.size() &&
        text.substr(text.size() - util::kRedactionMarker.size()) ==
            util::kRedactionMarker;
}

// The continuation of a redacted construct starts right after the marker
// that ended the last window; suppress_from_ tracks that position.
void RedactEmit::suppress_continuation() {
    if (suppression_ == Suppression::None) return;
    std::size_t end = suppress_from_;
    bool terminated = false;
    if (suppression_ == Suppression::QuotedValue) {
        while (end < pending_.size()) {
            if (pending_[end] == '\\' && end + 1 < pending_.size()) {
                end += 2;
                continue;
            }
            if (pending_[end] == quote_) {
                ++end; // consume the closing quote
                terminated = true;
                break;
            }
            ++end;
        }
        // Keep a consumed closing quote: it balances the value's opening
        // quote so the just-closed construct is not mistaken for an open
        // one when the window is reconsidered.
        pending_.erase(suppress_from_, end - suppress_from_ - (terminated ? 1 : 0));
    } else {
        while (end < pending_.size() &&
               !is_value_delimiter(pending_[end], authorization_)) {
            ++end;
        }
        terminated = end < pending_.size();
        pending_.erase(suppress_from_, end - suppress_from_);
    }
    if (terminated) {
        suppression_ = Suppression::None;
        authorization_ = false;
    } else {
        suppress_from_ = pending_.size();
    }
}

// A pending window that ends with a redaction marker may have cut a
// value or token mid-construct; suppress its continuation in later chunks.
void RedactEmit::arm_suppression() {
    if (!ends_with_marker(pending_)) return;
    const std::size_t marker_start = pending_.size() - util::kRedactionMarker.size();
    bool in_quote = false;
    char open_quote = '\0';
    for (std::size_t index = 0; index < marker_start; ++index) {
        const char ch = pending_[index];
        if (ch == '\\' && in_quote && index + 1 < marker_start) {
            ++index;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            if (!in_quote) {
                in_quote = true;
                open_quote = ch;
            } else if (ch == open_quote) {
                in_quote = false;
            }
        }
    }
    quote_ = open_quote;
    suppression_ = in_quote ? Suppression::QuotedValue : Suppression::UnquotedValue;
    authorization_ =
        suppression_ == Suppression::UnquotedValue && authorization_key_before(marker_start);
    suppress_from_ = pending_.size();
}

// Whether the assignment whose value ends at the marker belongs to an
// Authorization-looking key (quoted or plain), matching redact_assignments.
bool RedactEmit::authorization_key_before(std::size_t marker_start) const {
    const auto separator = pending_.find_last_of("=:", marker_start);
    if (separator == std::string::npos) return false;
    std::size_t key_end = separator;
    while (key_end > 0 && (pending_[key_end - 1] == ' ' || pending_[key_end - 1] == '\t')) {
        --key_end;
    }
    if (key_end == 0) return false;
    std::size_t key_begin = key_end;
    if (pending_[key_end - 1] == '"') {
        if (key_end < 2) return false;
        const auto open = pending_.rfind('"', key_end - 2);
        if (open == std::string::npos) return false;
        key_begin = open + 1;
        --key_end;
    } else {
        while (key_begin > 0 && key_end - key_begin < kMaxKeyBytes &&
               is_key_character(pending_[key_begin - 1])) {
            --key_begin;
        }
    }
    if (key_begin == key_end) return false;
    return util::normalized_secret_key(
               std::string_view(pending_).substr(key_begin, key_end - key_begin))
        .find("AUTHORIZATION") != std::string::npos;
}

// Length of the pending prefix that future chunks cannot change.
std::size_t RedactEmit::emit_length() const {
    std::size_t at_risk = extend_over_assignment(trailing_key_run_start());
    at_risk = std::min(at_risk, unmatched_quote_start());
    if (pending_.size() - at_risk > kMaxAtRiskBytes) {
        return pending_.size() - kMaxAtRiskBytes;
    }
    return at_risk;
}

std::size_t RedactEmit::trailing_key_run_start() const {
    std::size_t index = pending_.size();
    while (index > 0 && is_key_character(pending_[index - 1])) --index;
    return index;
}

// `key = value` / `key: value` context: extend the at-risk region back
// over the separator, surrounding spaces, and the candidate key.
std::size_t RedactEmit::extend_over_assignment(std::size_t at_risk) const {
    std::size_t index = at_risk;
    std::size_t spaces = 0;
    while (index > 0 && spaces < kMaxSeparatorSpaces &&
           (pending_[index - 1] == ' ' || pending_[index - 1] == '\t')) {
        --index;
        ++spaces;
    }
    if (index == 0 || (pending_[index - 1] != '=' && pending_[index - 1] != ':')) {
        return at_risk;
    }
    --index;
    spaces = 0;
    while (index > 0 && spaces < kMaxSeparatorSpaces &&
           (pending_[index - 1] == ' ' || pending_[index - 1] == '\t')) {
        --index;
        ++spaces;
    }
    const std::size_t key_end = index;
    if (index > 0 && pending_[index - 1] == '"') {
        if (index >= 2) {
            const auto open = pending_.rfind('"', index - 2);
            if (open != std::string::npos && index - open <= kMaxKeyBytes) return open;
        }
        return index;
    }
    while (index > 0 && key_end - index < kMaxKeyBytes &&
           is_key_character(pending_[index - 1])) {
        --index;
    }
    return index;
}

// An unmatched quote within the window may open a quoted secret value;
// hold back from it. Backslash-escaped quotes are ignored.
std::size_t RedactEmit::unmatched_quote_start() const {
    const std::size_t window_begin =
        pending_.size() > kMaxAtRiskBytes ? pending_.size() - kMaxAtRiskBytes : 0;
    std::optional<std::size_t> earliest;
    for (const char quote : {'"', '\''}) {
        bool in_quote = false;
        std::size_t open = 0;
        for (std::size_t index = window_begin; index < pending_.size(); ++index) {
            if (pending_[index] == '\\' && in_quote && index + 1 < pending_.size()) {
                ++index;
                continue;
            }
            if (pending_[index] != quote) continue;
            if (!in_quote) {
                in_quote = true;
                open = index;
            } else {
                in_quote = false;
            }
        }
        if (in_quote && (!earliest || open < *earliest)) earliest = open;
    }
    return earliest.value_or(pending_.size());
}

// --- Spill artifact ---------------------------------------------------------

util::ExpectedVoid SpillFile::start(
    std::string_view retained,
    std::string_view incoming) {
#if defined(__unix__) || defined(__APPLE__)
    std::error_code temp_error;
    const auto temp_directory = std::filesystem::temp_directory_path(temp_error);
    if (temp_error || temp_directory.empty()) {
        return std::unexpected(spill_error(
            "User Bash output spill temporary directory is unavailable",
            temp_error.message()));
    }
    for (int attempt = 0; attempt < 16; ++attempt) {
        const auto candidate =
            temp_directory / ("cch-user-bash-" + random_suffix() + ".log");
        harness::UniqueFd fd(::open(
            candidate.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
            0600));
        if (!fd) {
            if (errno == EEXIST) continue;
            return std::unexpected(spill_error(
                "could not create the User Bash output spill file",
                std::error_code(errno, std::generic_category()).message()));
        }
        if (auto written = write_all(fd.get(), retained); !written) {
            auto error = written.error();
            remove_candidate(fd, candidate);
            return std::unexpected(std::move(error));
        }
        if (auto written = write_all(fd.get(), incoming); !written) {
            auto error = written.error();
            remove_candidate(fd, candidate);
            return std::unexpected(std::move(error));
        }
        fd_ = std::move(fd);
        path_ = candidate;
        active_ = true;
        return {};
    }
    return std::unexpected(
        spill_error("could not allocate a unique User Bash output spill path", {}));
#else
    (void)retained;
    (void)incoming;
    return std::unexpected(
        spill_error("User Bash output spill is unavailable on this platform", {}));
#endif
}

util::ExpectedVoid SpillFile::write(std::string_view bytes) {
#if defined(__unix__) || defined(__APPLE__)
    return write_all(fd_.get(), bytes);
#else
    (void)bytes;
    return std::unexpected(
        spill_error("User Bash output spill is unavailable on this platform", {}));
#endif
}

util::ExpectedVoid SpillFile::finish() {
    if (!active_) return {};
    active_ = false;
#if defined(__unix__) || defined(__APPLE__)
    if (fd_.close() != 0) {
        const auto close_error = errno;
        remove_file();
        return std::unexpected(spill_error(
            "could not finish the User Bash output spill file",
            std::error_code(close_error, std::generic_category()).message()));
    }
#endif
    return {};
}

void SpillFile::abandon() {
    if (!active_) return;
    active_ = false;
#if defined(__unix__) || defined(__APPLE__)
    fd_.reset();
#endif
    remove_file();
}

util::Error SpillFile::spill_error(std::string message, std::string detail) {
    return util::make_error(
        util::ErrorCode::Process,
        std::move(message),
        bounded_redacted_presentation(std::move(detail)));
}

std::string SpillFile::random_suffix() {
    std::random_device random;
    std::string suffix;
    suffix.reserve(16);
    for (int index = 0; index < 8; ++index) {
        suffix += std::format("{:02x}", static_cast<unsigned>(random() & 0xffU));
    }
    return suffix;
}

#if defined(__unix__) || defined(__APPLE__)
util::ExpectedVoid SpillFile::write_all(int fd, std::string_view bytes) {
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto count = ::write(fd, bytes.data() + written, bytes.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            return std::unexpected(spill_error(
                "could not write the User Bash output spill file",
                std::error_code(errno, std::generic_category()).message()));
        }
        written += static_cast<std::size_t>(count);
    }
    return {};
}

void SpillFile::remove_candidate(
    harness::UniqueFd& fd,
    const std::filesystem::path& candidate) {
    fd.reset();
    std::error_code remove_error;
    std::filesystem::remove(candidate, remove_error);
}
#endif

void SpillFile::remove_file() {
    std::error_code remove_error;
    std::filesystem::remove(path_, remove_error);
}

} // namespace user_bash_output_detail

void UserBashOutputAccumulator::append(std::string_view raw) {
    if (raw.empty()) return;
    pump(raw, false);
}

void UserBashOutputAccumulator::finish() {
    pump({}, true);
    if (spill_.active()) {
        if (auto finished = spill_.finish(); !finished) {
            artifact_error_ = std::move(finished.error());
            return;
        }
        // The path is recorded only after creation and every write
        // succeeded; the artifact outlives Session Close.
        full_output_path_ = spill_.path().string();
    }
}

void UserBashOutputAccumulator::pump(std::string_view raw, bool flush) {
    std::string safe;
    safe.reserve(raw.size() + 8);
    utf8_.append(raw, safe);
    if (flush) utf8_.finish(safe);
    std::string stripped;
    ansi_.append(safe, stripped);
    if (flush) ansi_.finish(stripped);
    std::string normalized;
    carriage_return_.append(stripped, normalized);
    if (flush) carriage_return_.finish(normalized);
    std::string emitted;
    redact_.append(normalized, emitted);
    if (flush) redact_.finish(emitted);
    retain(emitted);
}

void UserBashOutputAccumulator::retain(const std::string& emitted) {
    if (emitted.empty()) return;
    if (spill_.active()) {
        if (auto written = spill_.write(emitted); !written) {
            auto error = std::move(written.error());
            spill_.abandon();
            artifact_error_ = std::move(error);
        }
    }
    if (!truncated_ && would_truncate(emitted)) {
        truncated_ = true;
        if (!artifact_error_) {
            if (auto started = spill_.start(tail_, emitted); !started) {
                artifact_error_ = std::move(started.error());
            }
        }
    }
    const std::size_t window = limit_.max_bytes + kBoundarySlack;
    if (tail_.size() + emitted.size() > window) {
        // The surviving region always lies within the last window bytes;
        // pre-cut older bytes so retained memory stays bounded. The slack
        // keeps a split redaction marker or multibyte sequence adjacent to
        // the final cut point intact for the trim below.
        std::size_t excess = tail_.size() + emitted.size() - window;
        if (excess < tail_.size()) {
            tail_.erase(0, excess);
            tail_.append(emitted);
        } else {
            excess -= tail_.size();
            tail_.assign(emitted, excess, std::string::npos);
        }
        trim_tail();
        return;
    }
    tail_.append(emitted);
    trim_tail();
}

bool UserBashOutputAccumulator::would_truncate(const std::string& emitted) const {
    if (tail_.size() + emitted.size() > limit_.max_bytes) return true;
    const auto newlines = std::count(emitted.begin(), emitted.end(), '\n');
    return tail_newlines() + newlines >= limit_.max_lines;
}

std::size_t UserBashOutputAccumulator::tail_newlines() const {
    return static_cast<std::size_t>(std::count(tail_.begin(), tail_.end(), '\n'));
}

void UserBashOutputAccumulator::trim_tail() {
    std::size_t start = tail_.size();
    std::size_t bytes = 0;
    std::size_t lines = 1;
    while (start > 0 && bytes < limit_.max_bytes) {
        const char ch = tail_[start - 1];
        if (ch == '\n' && lines >= limit_.max_lines) break;
        --start;
        ++bytes;
        if (ch == '\n') ++lines;
    }
    if (start == 0) return;
    while (start < tail_.size() &&
           (static_cast<unsigned char>(tail_[start]) & 0xc0) == 0x80) {
        ++start;
    }
    const auto marker = tail_.rfind(util::kRedactionMarker, start);
    if (marker != std::string::npos && marker < start &&
        marker + util::kRedactionMarker.size() > start) {
        start = marker + util::kRedactionMarker.size();
    }
    truncated_ = true;
    tail_.erase(0, start);
}

} // namespace cch::coding_agent::runtime
