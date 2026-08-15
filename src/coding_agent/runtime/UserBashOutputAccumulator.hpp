#pragma once

// Incremental User Bash output pipeline following pi's recipe (ADR 0028). Raw
// stdout/stderr bytes arrive in callback-arrival order and pass through UTF-8
// safety, ANSI stripping, and binary-garbage filtering with carriage-return
// removal — each stage holding back only the bounded partial construct that
// may continue in a later chunk. No secret redaction is applied anywhere in
// the output path. The sanitized stream feeds a bounded rolling tail (live
// presentation and the completed Bash Message) and, once the tail truncates,
// a complete spill file in the OS temporary directory.

#include "support/UniqueFd.hpp"
#include "harness/OutputLimiter.hpp"

#include <cch/support/Error.hpp>

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
// invalid byte becomes U+FFFD, matching ai::bounded_utf8 whole-buffer rules.

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
// Mirrors tools::strip_terminal_escape_sequences while holding back an
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

// --- Stage 3: binary-garbage filter and carriage-return removal ------------
// Pi's sanitizeBinaryOutput + .replace(/\r/g, ""): carriage returns are
// removed outright (CRLF collapses to LF, a lone CR disappears), C0 controls
// other than "\n"/"\t" and U+FFF9–U+FFFB are dropped, and tab, LF, and DEL
// are preserved. Stateless: stage 1 emits complete UTF-8 sequences only, so
// no chunk-boundary hold-back is needed.

class ControlFilter {
public:
    void append(std::string_view text, std::string& out);
};

// --- Spill artifact ---------------------------------------------------------
// The complete sanitized stream written incrementally to a unique owner-only
// file in the OS temporary directory once the retained tail truncates. The
// path is surfaced only after every write and the final close succeed; any
// failure abandons (and removes) the partial artifact without erasing the
// bounded truncated result.

class SpillFile {
public:
    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    // Creates the artifact and writes the complete stream so far: the
    // retained tail plus the arriving increment.
    [[nodiscard]] support::ExpectedVoid start(
        std::string_view retained,
        std::string_view incoming);
    [[nodiscard]] support::ExpectedVoid write(std::string_view bytes);
    // Flushes ownership of the artifact to the caller-visible state: after a
    // successful close the path() is valid and persistent.
    [[nodiscard]] support::ExpectedVoid finish();
    // A failed artifact is incomplete and misleading: close and remove it.
    void abandon();

private:
    [[nodiscard]] static support::Error spill_error(std::string message, std::string detail);
    [[nodiscard]] static std::string random_suffix();
#if defined(__unix__) || defined(__APPLE__)
    [[nodiscard]] static support::ExpectedVoid write_all(int fd, std::string_view bytes);
    static void remove_candidate(
        support::UniqueFd& fd,
        const std::filesystem::path& candidate);
#endif
    void remove_file();

#if defined(__unix__) || defined(__APPLE__)
    support::UniqueFd fd_;
#endif
    std::filesystem::path path_;
    bool active_{false};
};

} // namespace user_bash_output_detail

/// Incremental, bounded User Bash output pipeline. Confined to the owning
/// Agent Session runtime executor; not thread-safe.
class UserBashOutputAccumulator {
public:
    explicit UserBashOutputAccumulator(harness::OutputLimit limit = {}) : limit_(limit) {}

    /// Feed one raw stdout/stderr chunk in callback-arrival order. The
    /// sanitized increment is reflected in tail() on return.
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
    [[nodiscard]] const std::optional<support::Error>& artifact_error() const {
        return artifact_error_;
    }

private:
    // Push raw bytes (or, when flush is set, each stage's held-back partial
    // construct) through the three-stage pipeline into the rolling tail.
    void pump(std::string_view raw, bool flush);
    // Retain the sanitized increment in the rolling tail, applying the same
    // tail semantics as harness::limit_output_tail on the complete stream. Once
    // the tail truncates, the complete sanitized stream is spilled
    // incrementally; a spill failure preserves the bounded truncated result
    // and records a bounded redacted diagnostic instead of a path.
    void retain(const std::string& emitted);
    // Mirrors the trim below: the complete stream exceeds the retained limits
    // exactly when its bytes exceed max_bytes or its lines exceed max_lines.
    [[nodiscard]] bool would_truncate(const std::string& emitted) const;
    [[nodiscard]] std::size_t tail_newlines() const;
    // Backward-walk trim identical to harness::limit_output_tail: at most
    // limit_.max_bytes bytes and limit_.max_lines lines, never splitting a
    // multibyte sequence at the cut point.
    void trim_tail();

    // One maximum-length UTF-8 sequence: keeps the lead byte of a sequence
    // straddling the pre-cut window boundary available to the trim below.
    static constexpr std::size_t kBoundarySlack{4};
    harness::OutputLimit limit_;
    user_bash_output_detail::Utf8Safety utf8_;
    user_bash_output_detail::AnsiStrip ansi_;
    user_bash_output_detail::ControlFilter control_filter_;
    user_bash_output_detail::SpillFile spill_;
    std::string tail_;
    bool truncated_{false};
    std::optional<std::string> full_output_path_;
    std::optional<support::Error> artifact_error_;
};

} // namespace cch::coding_agent::runtime
