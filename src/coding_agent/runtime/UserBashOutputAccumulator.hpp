#pragma once

// Incremental User Bash output pipeline. Raw stdout/stderr bytes arrive in
// callback-arrival order and pass through UTF-8 safety, ANSI stripping,
// carriage-return normalization, and secret redaction — each stage holding
// back only the bounded partial construct that may continue in a later chunk.
// The sanitized stream feeds a bounded rolling tail (live presentation and the
// completed Bash Message) and, once the tail truncates, a complete spill file
// in the OS temporary directory.

#include "harness/UniqueFd.hpp"
#include "util/OutputLimiter.hpp"
#include "util/Redactor.hpp"

#include <cch/util/Error.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace cch::coding_agent::runtime {

namespace user_bash_output_detail {

inline constexpr std::string_view kUtf8Replacement{"\xef\xbf\xbd"};

// --- Stage 1: incremental UTF-8 safety -------------------------------------
// Holds back a trailing incomplete-but-plausible multibyte prefix; every other
// invalid byte becomes U+FFFD, matching util::bounded_utf8 whole-buffer rules.

class Utf8Safety {
public:
    void append(std::string_view raw, std::string& out);
    void finish(std::string& out);

private:
    [[nodiscard]] static std::size_t sequence_length(unsigned char lead);
    [[nodiscard]] static bool plausible_prefix(std::string_view rest);
    [[nodiscard]] static bool valid_sequence(
        std::string_view text,
        std::size_t index,
        std::size_t length);

    std::string buffer_;
};

// --- Stage 2: incremental ANSI stripping ------------------------------------
// Mirrors util::strip_terminal_escape_sequences while holding back an
// unterminated escape. An unterminated sequence longer than kMaxEscapeBytes is
// dropped outright: over-stripping is safe, emitting partial controls is not.
// (Bounded memory requires the cap; the whole-buffer stripper would consume
// the sequence to end of input.)

class AnsiStrip {
public:
    void append(std::string_view text, std::string& out);
    void finish(std::string& out);

private:
    [[nodiscard]] std::size_t sequence_end(std::size_t esc) const;

    static constexpr std::size_t kMaxEscapeBytes{4096};
    std::string buffer_;
};

// --- Stage 3: carriage-return normalization and binary control dropping -----
// "\r\n" and a lone "\r" both become "\n"; controls below 0x20 other than
// "\n"/"\t" are dropped. A trailing "\r" is held back one chunk.

class CarriageReturnNormalize {
public:
    void append(std::string_view text, std::string& out);
    void finish(std::string& out);

private:
    bool held_cr_{false};
};

// --- Stage 4: incremental secret redaction -----------------------------------
// Runs the single whole-buffer redactor (CODING_STANDARDS §10.1) over a
// bounded pending window and emits only the prefix that future chunks cannot
// change: the trailing at-risk construct (secret-key candidate, token prefix,
// or unmatched quoted value) is held back for the next chunk. A redaction
// marker at the end of the window arms continuation suppression so the tail
// of a value or token redacted mid-construct is dropped rather than leaked.
// Bounded-window divergence: secret constructs longer than kMaxAtRiskBytes
// (about 4 KiB) or keys longer than kMaxKeyBytes may not fully redact; the
// whole-buffer redactor has no such window.

class RedactEmit {
public:
    void append(std::string_view text, std::string& out);
    void finish(std::string& out);

private:
    enum class Suppression { None, UnquotedValue, QuotedValue };

    [[nodiscard]] static bool is_key_character(char ch);
    [[nodiscard]] static bool is_value_delimiter(char ch, bool authorization);
    [[nodiscard]] static bool ends_with_marker(std::string_view text);
    void suppress_continuation();
    void arm_suppression();
    [[nodiscard]] bool authorization_key_before(std::size_t marker_start) const;
    [[nodiscard]] std::size_t emit_length() const;
    [[nodiscard]] std::size_t trailing_key_run_start() const;
    [[nodiscard]] std::size_t extend_over_assignment(std::size_t at_risk) const;
    [[nodiscard]] std::size_t unmatched_quote_start() const;

    static constexpr std::size_t kMaxAtRiskBytes{4096};
    static constexpr std::size_t kMaxKeyBytes{256};
    static constexpr std::size_t kMaxSeparatorSpaces{8};
    std::string pending_;
    Suppression suppression_{Suppression::None};
    std::size_t suppress_from_{0};
    char quote_{'\0'};
    bool authorization_{false};
};

// --- Spill artifact ---------------------------------------------------------
// The complete sanitized, redacted stream written incrementally to a unique
// owner-only file in the OS temporary directory once the retained tail
// truncates. The path is surfaced only after every write and the final close
// succeed; any failure abandons (and removes) the partial artifact without
// erasing the bounded truncated result.

class SpillFile {
public:
    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    // Creates the artifact and writes the complete stream so far: the
    // retained tail plus the arriving increment.
    [[nodiscard]] util::ExpectedVoid start(
        std::string_view retained,
        std::string_view incoming);
    [[nodiscard]] util::ExpectedVoid write(std::string_view bytes);
    // Flushes ownership of the artifact to the caller-visible state: after a
    // successful close the path() is valid and persistent.
    [[nodiscard]] util::ExpectedVoid finish();
    // A failed artifact is incomplete and misleading: close and remove it.
    void abandon();

private:
    [[nodiscard]] static util::Error spill_error(std::string message, std::string detail);
    [[nodiscard]] static std::string random_suffix();
#if defined(__unix__) || defined(__APPLE__)
    [[nodiscard]] static util::ExpectedVoid write_all(int fd, std::string_view bytes);
    static void remove_candidate(
        harness::UniqueFd& fd,
        const std::filesystem::path& candidate);
#endif
    void remove_file();

#if defined(__unix__) || defined(__APPLE__)
    harness::UniqueFd fd_;
#endif
    std::filesystem::path path_;
    bool active_{false};
};

} // namespace user_bash_output_detail

/// Incremental, bounded User Bash output pipeline. Confined to the owning
/// Agent Session runtime executor; not thread-safe.
class UserBashOutputAccumulator {
public:
    explicit UserBashOutputAccumulator(util::OutputLimit limit = {}) : limit_(limit) {}

    /// Feed one raw stdout/stderr chunk in callback-arrival order. The
    /// sanitized, redacted increment is reflected in tail() on return.
    void append(std::string_view raw);

    /// No more output will arrive: flush every stage's held-back partial
    /// construct, then finish the spill artifact if one is active.
    void finish();

    /// The execution failed or will not complete normally: drop any partial
    /// spill artifact. The retained tail is left intact for diagnostics.
    void discard() { spill_.abandon(); }

    /// The bounded rolling tail: at most limit_ lines or bytes, UTF-8 safe.
    [[nodiscard]] const std::string& tail() const { return tail_; }
    [[nodiscard]] bool truncated() const { return truncated_; }
    /// Set only after the complete sanitized stream was written successfully.
    [[nodiscard]] const std::optional<std::string>& full_output_path() const {
        return full_output_path_;
    }
    /// A spill failure never erases the bounded truncated result.
    [[nodiscard]] const std::optional<util::Error>& artifact_error() const {
        return artifact_error_;
    }

private:
    // Push raw bytes (or, when flush is set, each stage's held-back partial
    // construct) through the four-stage pipeline into the rolling tail.
    void pump(std::string_view raw, bool flush);
    // Retain the sanitized increment in the rolling tail, applying the same
    // tail semantics as util::limit_output_tail on the complete stream. Once
    // the tail truncates, the complete sanitized stream is spilled
    // incrementally; a spill failure preserves the bounded truncated result
    // and records a bounded redacted diagnostic instead of a path.
    void retain(const std::string& emitted);
    // Mirrors the trim below: the complete stream exceeds the retained limits
    // exactly when its bytes exceed max_bytes or its lines exceed max_lines.
    [[nodiscard]] bool would_truncate(const std::string& emitted) const;
    [[nodiscard]] std::size_t tail_newlines() const;
    // Backward-walk trim identical to util::limit_output_tail: at most
    // limit_.max_bytes bytes and limit_.max_lines lines, never splitting a
    // multibyte sequence or a redaction marker at the cut point.
    void trim_tail();

    static constexpr std::size_t kBoundarySlack{util::kRedactionMarker.size() + 4};
    util::OutputLimit limit_;
    user_bash_output_detail::Utf8Safety utf8_;
    user_bash_output_detail::AnsiStrip ansi_;
    user_bash_output_detail::CarriageReturnNormalize carriage_return_;
    user_bash_output_detail::RedactEmit redact_;
    user_bash_output_detail::SpillFile spill_;
    std::string tail_;
    bool truncated_{false};
    std::optional<std::string> full_output_path_;
    std::optional<util::Error> artifact_error_;
};

} // namespace cch::coding_agent::runtime
