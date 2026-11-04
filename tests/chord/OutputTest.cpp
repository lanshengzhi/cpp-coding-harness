#include <cch/chord/Output.hpp>

#include <cch/support/JsonAccess.hpp>

#include "support/ChordEvidence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;
using namespace cch::chord;

namespace {

/// The frozen #1057 json-output-support observation the receipts register.
[[nodiscard]] cch::tests::FrozenChordScenario json_output_scenario(std::string_view name) {
    auto scenario = cch::tests::FrozenChordScenario::load("audit-1057", "json-output-support.json", name);
    REQUIRE(scenario.has_value());
    return *scenario;
}

const std::string kMixed = "a\xc3\xa9\xf0\x9f\x98\x80"; // "aé😀"

using namespace std::chrono_literals;

/// One step of a declared progress schedule: coalescing marks at the current
/// time, then an optional advance that runs every commit the pacer allows.
struct ProgressStep {
    int marks{0};
    std::optional<ProgressTime> advance{};
};

[[nodiscard]] ProgressStep mark_step(int times = 1) { return ProgressStep{.marks = times, .advance = std::nullopt}; }

[[nodiscard]] ProgressStep advance_step(double milliseconds) {
    return ProgressStep{.marks = 0, .advance = ProgressTime{milliseconds}};
}

struct ProgressCommit {
    ProgressTime at;
    std::size_t bytes{0};
};

/// Drive the pacer with the same declared schedule the frozen capture uses.
/// The caller's virtual clock starts at zero and only the steps move it.
[[nodiscard]] std::vector<ProgressCommit> run_progress_schedule(std::chrono::milliseconds min_interval,
        const std::vector<ProgressStep>& steps,
        const std::vector<std::size_t>& write_bytes) {
    ProgressPacer pacer{min_interval};
    std::vector<ProgressCommit> commits;
    ProgressTime now{};
    std::size_t next_write = 0;
    const auto commit_due = [&](ProgressTime target) {
        while (pacer.pending() && pacer.due(target) && next_write < write_bytes.size()) {
            now = std::max(now, pacer.next_commit_at());
            pacer.begin();
            pacer.committed(now, write_bytes[next_write]);
            commits.push_back(ProgressCommit{.at = now, .bytes = write_bytes[next_write]});
            ++next_write;
        }
    };
    for (const auto& step : steps) {
        for (int count = 0; count < step.marks; ++count)
            pacer.mark();
        if (step.advance) {
            const ProgressTime target = now + *step.advance;
            commit_due(target);
            now = target;
        }
    }
    commit_due(now);
    return commits;
}

} // namespace

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/json-output-support.json
// "UTF-8 byte length and character-end boundaries match the frozen output helpers". The frozen
// text, its byte lengths and its boundary table drive every assertion below.
TEST_CASE("UTF-8 byte length and character-end boundaries match the frozen output helpers",
        "[chord][output][utf8][issue1060][spec]") {
    const auto frozen =
            json_output_scenario("UTF-8 byte length and character-end boundaries match the frozen output helpers");
    const auto text = frozen.inputs_string("text");
    REQUIRE(text.has_value());
    const auto* frozen_lengths = frozen.object_member("lengths");
    const auto* frozen_boundaries = frozen.array_member("boundaries");
    REQUIRE(frozen_lengths != nullptr);
    REQUIRE(frozen_boundaries != nullptr);
    const auto ascii = support::json_number_member(*frozen_lengths, "ascii");
    const auto two_byte = support::json_number_member(*frozen_lengths, "twoByte");
    const auto four_byte = support::json_number_member(*frozen_lengths, "fourByte");
    const auto mixed = support::json_number_member(*frozen_lengths, "mixed");
    REQUIRE(ascii.has_value());
    REQUIRE(two_byte.has_value());
    REQUIRE(four_byte.has_value());
    REQUIRE(mixed.has_value());

    CHECK(utf8_byte_length(*text) == static_cast<std::size_t>(*mixed));
    const auto two_byte_text = text->substr(1, 2);
    const auto four_byte_text = text->substr(3, 4);
    CHECK(utf8_byte_length(two_byte_text) == static_cast<std::size_t>(*two_byte));
    CHECK(utf8_byte_length(four_byte_text) == static_cast<std::size_t>(*four_byte));
    // Frozen ascii length: a pure-ASCII sample's byte length equals its length.
    CHECK(utf8_byte_length("abc") == static_cast<std::size_t>(*ascii));

    REQUIRE(frozen_boundaries->size() == text->size() + 1);
    for (std::size_t index = 0; index < frozen_boundaries->size(); ++index) {
        CHECK(character_end(*text, index) == static_cast<std::size_t>(frozen_boundaries->at(index).get<double>()));
    }
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/json-output-support.json
// "boundOutput and OutputBuffer retain the configured head or tail and report exact dropped
// counts". The frozen text and limits drive the head/tail bound checks and the frozen head/tail
// expected records are compared exactly.
TEST_CASE("bound output keeps the configured head or tail and reports exact dropped counts",
        "[chord][output][bound][issue1060][spec]") {
    const auto frozen = json_output_scenario(
            "boundOutput and OutputBuffer retain the configured head or tail and report exact dropped counts");
    const auto text = frozen.inputs_string("text");
    REQUIRE(text.has_value());
    const auto* frozen_limits = frozen.inputs_member("limits");
    REQUIRE(frozen_limits != nullptr);
    const auto* limits_object = frozen_limits->get_if<support::JsonValue::object_t>();
    REQUIRE(limits_object != nullptr);
    const auto max_bytes = support::json_number_member(*limits_object, "maxBytes");
    const auto max_lines = support::json_number_member(*limits_object, "maxLines");
    REQUIRE(max_bytes.has_value());
    REQUIRE(max_lines.has_value());
    const OutputLimits limits{
            static_cast<std::size_t>(*max_bytes), static_cast<std::size_t>(*max_lines), OutputRetention::Head};

    const auto check_bound = [&text](const support::JsonValue::object_t& frozen_record, const OutputSlice& actual) {
        const auto frozen_text = support::json_string_member(frozen_record, "text");
        const auto bytes = support::json_number_member(frozen_record, "bytes");
        const auto dropped_bytes = support::json_number_member(frozen_record, "droppedBytes");
        const auto dropped_lines = support::json_number_member(frozen_record, "droppedLines");
        REQUIRE(frozen_text.has_value());
        REQUIRE(bytes.has_value());
        REQUIRE(dropped_bytes.has_value());
        REQUIRE(dropped_lines.has_value());
        CHECK(actual.text == *frozen_text);
        CHECK(actual.bytes == static_cast<std::size_t>(*bytes));
        CHECK(actual.dropped_bytes == static_cast<std::size_t>(*dropped_bytes));
        CHECK(actual.dropped_lines == static_cast<std::size_t>(*dropped_lines));
        CHECK(actual.text.size() == actual.bytes);
        CHECK(actual.dropped_bytes == text->size() - actual.bytes);
    };

    const auto* frozen_head = frozen.object_member("head");
    const auto* frozen_tail = frozen.object_member("tail");
    REQUIRE(frozen_head != nullptr);
    REQUIRE(frozen_tail != nullptr);
    check_bound(*frozen_head, bound_output(*text, limits));
    check_bound(
            *frozen_tail, bound_output(*text, OutputLimits{limits.max_bytes, limits.max_lines, OutputRetention::Tail}));

    const auto head_lines = bound_output("one\ntwo\nthree", OutputLimits{100, 2, OutputRetention::Head});
    CHECK(head_lines.text == "one\ntwo\n");
    CHECK(head_lines.bytes == 8);
    CHECK(head_lines.dropped_bytes == 5);
    CHECK(head_lines.dropped_lines == 1);

    const auto tail_lines = bound_output("one\ntwo\nthree", OutputLimits{100, 2, OutputRetention::Tail});
    CHECK(tail_lines.text == "two\nthree");
    CHECK(tail_lines.bytes == 9);
    CHECK(tail_lines.dropped_bytes == 4);
    CHECK(tail_lines.dropped_lines == 1);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/json-output-support.json
// "UTF-8 byte length and character-end boundaries match the frozen output helpers": the frozen
// boundary table is the character-cut contract. A head or tail bound that lands inside a
// multi-byte character keeps the frozen character boundary prefix or suffix, never a partial
// character.
TEST_CASE("a multi-byte character is cut only on a character boundary", "[chord][output][utf8][issue1060][spec]") {
    const auto frozen =
            json_output_scenario("UTF-8 byte length and character-end boundaries match the frozen output helpers");
    const auto text = frozen.inputs_string("text");
    const auto* frozen_boundaries = frozen.array_member("boundaries");
    REQUIRE(text.has_value());
    REQUIRE(frozen_boundaries != nullptr);
    REQUIRE(text->size() + 1 == frozen_boundaries->size());
    const auto boundary_at = [&](std::size_t index) {
        return static_cast<std::size_t>(frozen_boundaries->at(index).get<double>());
    };
    // The frozen boundary table lists the start offset of the character containing each byte.
    const auto head_prefix = [&](std::size_t limit) {
        std::size_t best = 0;
        for (std::size_t index = 0; index < frozen_boundaries->size(); ++index) {
            const auto start = boundary_at(index);
            if (start <= limit && start > best) best = start;
        }
        return best;
    };
    const auto tail_start = [&](std::size_t limit) {
        const std::size_t threshold = text->size() - limit;
        std::size_t best = text->size();
        for (std::size_t index = 0; index < frozen_boundaries->size(); ++index) {
            const auto start = boundary_at(index);
            if (start >= threshold && start < best) best = start;
        }
        return best;
    };

    const auto head_two = bound_output(*text, OutputLimits{2, 1, OutputRetention::Head});
    CHECK(head_two.text == text->substr(0, head_prefix(2)));
    CHECK(head_two.text == "a");
    CHECK(head_two.bytes == 1);
    CHECK(head_two.dropped_bytes == 6);

    const auto head_three = bound_output(*text, OutputLimits{3, 1, OutputRetention::Head});
    CHECK(head_three.text == text->substr(0, head_prefix(3)));
    CHECK(head_three.text == "a\xc3\xa9");
    CHECK(head_three.bytes == 3);
    CHECK(head_three.dropped_bytes == 4);

    const auto tail_four = bound_output(*text, OutputLimits{4, 1, OutputRetention::Tail});
    CHECK(tail_four.text == text->substr(tail_start(4)));
    CHECK(tail_four.text == "\xf0\x9f\x98\x80");
    CHECK(tail_four.bytes == 4);
    CHECK(tail_four.dropped_bytes == 3);

    const auto tail_two = bound_output(*text, OutputLimits{2, 1, OutputRetention::Tail});
    CHECK(tail_two.text == text->substr(tail_start(2)));
    CHECK(tail_two.text.empty());
    CHECK(tail_two.bytes == 0);
    CHECK(tail_two.dropped_bytes == 7);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/json-output-support.json
// "boundOutput and OutputBuffer retain the configured head or tail and report exact dropped counts"
// (pushes "abc", "def", "gh"; snapshot text "efgh"). The frozen pushes drive the buffer and the
// frozen snapshot/accepted fields are compared exactly.
TEST_CASE("output buffer retains the frozen tail window and exact whole-stream totals",
        "[chord][output][buffer][issue1060][spec]") {
    const auto frozen = json_output_scenario(
            "boundOutput and OutputBuffer retain the configured head or tail and report exact dropped counts");
    const auto* frozen_pushes = frozen.inputs_member("pushes");
    const auto* frozen_accepted = frozen.array_member("accepted");
    const auto* frozen_snapshot = frozen.object_member("snapshot");
    REQUIRE(frozen_pushes != nullptr);
    REQUIRE(frozen_accepted != nullptr);
    REQUIRE(frozen_snapshot != nullptr);
    const auto* pushes = frozen_pushes->get_if<support::JsonValue::array_t>();
    REQUIRE(pushes != nullptr);
    REQUIRE(pushes->size() == frozen_accepted->size());

    OutputBuffer buffer(OutputLimits{4, 1, OutputRetention::Tail});
    for (std::size_t index = 0; index < pushes->size(); ++index) {
        const auto* push = pushes->at(index).get_if<std::string>();
        const auto* accepted = frozen_accepted->at(index).get_if<bool>();
        REQUIRE(push != nullptr);
        REQUIRE(accepted != nullptr);
        CHECK(buffer.push(*push) == *accepted);
    }

    const auto snapshot = buffer.snapshot();
    const auto frozen_text = support::json_string_member(*frozen_snapshot, "text");
    const auto dropped_bytes = support::json_number_member(*frozen_snapshot, "droppedBytes");
    const auto dropped_lines = support::json_number_member(*frozen_snapshot, "droppedLines");
    REQUIRE(frozen_text.has_value());
    REQUIRE(dropped_bytes.has_value());
    REQUIRE(dropped_lines.has_value());
    CHECK(snapshot.text == *frozen_text);
    CHECK(snapshot.dropped_bytes == static_cast<std::size_t>(*dropped_bytes));
    CHECK(snapshot.dropped_lines == static_cast<std::size_t>(*dropped_lines));
}

TEST_CASE("output buffer keeps whole-stream dropped line counts across a tail window",
        "[chord][output][buffer][issue1060][spec]") {
    // The frozen observation is the whole-stream accounting contract
    // (fixtures/pi-v1.3.0/audit-1057/json-output-support.json): its snapshot's dropped byte
    // count equals the pushed bytes minus the retained window across the whole push sequence.
    const auto frozen = json_output_scenario(
            "boundOutput and OutputBuffer retain the configured head or tail and report exact dropped counts");
    const auto* frozen_pushes = frozen.inputs_member("pushes");
    const auto* frozen_snapshot = frozen.object_member("snapshot");
    REQUIRE(frozen_pushes != nullptr);
    REQUIRE(frozen_snapshot != nullptr);
    const auto* pushes = frozen_pushes->get_if<support::JsonValue::array_t>();
    const auto frozen_dropped_bytes = support::json_number_member(*frozen_snapshot, "droppedBytes");
    const auto frozen_text = support::json_string_member(*frozen_snapshot, "text");
    REQUIRE(pushes != nullptr);
    REQUIRE(frozen_dropped_bytes.has_value());
    REQUIRE(frozen_text.has_value());
    std::size_t frozen_pushed_bytes = 0;
    for (const auto& push : *pushes) {
        const auto* bytes = push.get_if<std::string>();
        REQUIRE(bytes != nullptr);
        frozen_pushed_bytes += bytes->size();
    }
    CHECK(frozen_pushed_bytes - frozen_text->size() == static_cast<std::size_t>(*frozen_dropped_bytes));

    OutputBuffer buffer(OutputLimits{5, 1, OutputRetention::Tail});

    CHECK(buffer.push("one\n"));
    CHECK(buffer.push("two\n"));
    CHECK(buffer.push("three"));

    const auto snapshot = buffer.snapshot();
    CHECK(snapshot.text == "three");
    // The same whole-stream identity with dropped lines: 13 pushed bytes, 5 retained, 2
    // complete lines dropped across the stream.
    CHECK(snapshot.dropped_bytes == 13 - snapshot.text.size());
    CHECK(snapshot.dropped_bytes == 8);
    CHECK(snapshot.dropped_lines == 2);
}

// Frozen observation at 71023aec1546d6d677a65669c5665c0927ce0dd0:
// packages/chord/src/output.ts:boundOutput returns the exact unsanitized slice.
TEST_CASE("bound output keeps the exact unsanitized slice", "[chord][output][bound][sanitize][issue1044][spec]") {
    const std::string raw{"a\0b\tc\nd", 7};
    const auto slice = bound_output(raw, OutputLimits{100, 10, OutputRetention::Head});

    CHECK(slice.text == raw);
    CHECK(slice.text.size() == 7);
    CHECK(slice.bytes == 7);
    CHECK(slice.dropped_bytes == 0);
    CHECK(slice.dropped_lines == 0);
}

// Frozen observation at 71023aec1546d6d677a65669c5665c0927ce0dd0:
// packages/chord/src/output.ts:sanitizeOutput removes [\x00-\x08\x0b-\x1f\ufff9-\ufffb]
// and OutputBuffer.snapshot sanitizes only text, never the raw dropped-byte accounting.
TEST_CASE("output buffer snapshot sanitizes control characters without changing raw accounting",
        "[chord][output][buffer][sanitize][issue1044][spec]") {
    OutputBuffer head(OutputLimits{100, 10, OutputRetention::Head});
    CHECK(head.push(std::string{"a\0b\t", 4}));
    CHECK(head.push("c\xef\xbf\xbb"
                    "d\ne\x0b"
                    "f"));

    const auto head_snapshot = head.snapshot();
    CHECK(head_snapshot.text == "ab\tcd\nef");
    CHECK(head_snapshot.dropped_bytes == 0);
    CHECK(head_snapshot.dropped_lines == 0);

    OutputBuffer tail(OutputLimits{5, 1, OutputRetention::Tail});
    CHECK(tail.push(std::string{"d\0ef\n", 5}));

    const auto tail_snapshot = tail.snapshot();
    CHECK(tail_snapshot.text == "def\n");
    CHECK(tail_snapshot.text.size() == 4);
    CHECK(tail_snapshot.dropped_bytes == 0);
    CHECK(tail_snapshot.dropped_lines == 0);
}

// Frozen observation at 71023aec1546d6d677a65669c5665c0927ce0dd0:
// packages/chord/src/output.ts:sanitizeOutput removes control characters but keeps
// tabs, newlines and other text (frozen output.test.ts sanitize case).
TEST_CASE("output buffer snapshot matches the frozen sanitize output case",
        "[chord][output][buffer][sanitize][issue1044][spec]") {
    std::string input{"a\0b\tc\nd\re\a", 10};
    input += "f\xef\xbf\xb9";
    input += "g\xef\xbf\xbb";
    input += "h\xf0\x9f\x98\x80";

    OutputBuffer buffer(OutputLimits{1000, 10, OutputRetention::Head});
    CHECK(buffer.push(input));
    CHECK(buffer.snapshot().text == "ab\tc\ndefgh\xf0\x9f\x98\x80");
}

// Frozen observation at 71023aec1546d6d677a65669c5665c0927ce0dd0:
// packages/chord/src/output.ts:OutputBuffer.snapshot sanitizes the retained window
// but counts the raw stream (frozen output.test.ts raw-stream case).
TEST_CASE("output buffer snapshot sanitizes each retained window while counting the raw stream",
        "[chord][output][buffer][sanitize][issue1044][spec]") {
    OutputBuffer buffer(OutputLimits{1000, 1, OutputRetention::Tail});

    CHECK(buffer.push("a\a\n"));
    const auto first = buffer.snapshot();
    CHECK(first.text == "a\n");
    CHECK(first.dropped_bytes == 0);
    CHECK(first.dropped_lines == 0);

    CHECK(buffer.push("b\x1b\n"));
    const auto second = buffer.snapshot();
    CHECK(second.text == "b\n");
    CHECK(second.dropped_bytes == 3);
    CHECK(second.dropped_lines == 1);
}

// Frozen observation at 71023aec1546d6d677a65669c5665c0927ce0dd0:
// packages/chord/src/output.ts:sanitizeOutput removes U+FFF9..U+FFFB but keeps
// U+FFFC, tabs, newlines and every byte of a multibyte character.
TEST_CASE("output buffer snapshot removes interlinear controls and preserves multibyte text",
        "[chord][output][buffer][sanitize][issue1044][spec]") {
    OutputBuffer marks(OutputLimits{100, 10, OutputRetention::Tail});
    CHECK(marks.push("a\xef\xbf\xb9"
                     "b\xef\xbf\xba"
                     "c\xef\xbf\xbb"
                     "d\xef\xbf\xbc"
                     "e"));

    const auto marks_snapshot = marks.snapshot();
    CHECK(marks_snapshot.text == "abcd\xef\xbf\xbc"
                                 "e");
    CHECK(marks_snapshot.text.size() == 8);
    CHECK(marks_snapshot.dropped_bytes == 0);

    OutputBuffer multibyte(OutputLimits{100, 10, OutputRetention::Tail});
    CHECK(multibyte.push("a\xc3\xa9\xf0\x9f\x98\x80\xef\xbf\xba\tend\n"));

    const auto multibyte_snapshot = multibyte.snapshot();
    CHECK(multibyte_snapshot.text == "a\xc3\xa9\xf0\x9f\x98\x80\tend\n");
    CHECK(multibyte_snapshot.dropped_bytes == 0);
    CHECK(multibyte_snapshot.dropped_lines == 0);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/json-output-support.json
// "the progress byte rate is the frozen 100 KiB/s constant" (expected bytesPerSecond 102400).
TEST_CASE("the progress byte rate is the frozen 100 KiB/s constant", "[chord][output][progress][issue1187][spec]") {
    const auto frozen = json_output_scenario("the progress byte rate is the frozen 100 KiB/s constant");
    const auto bytes_per_second = frozen.number_member("bytesPerSecond");
    REQUIRE(bytes_per_second.has_value());
    CHECK(kProgressBytesPerSecond == static_cast<std::size_t>(*bytes_per_second));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1187/chord-progress.json
// "progress commits the first change at once then paces later commits by their written bytes".
// The declared inputs are minIntervalMs 100, writeBytes 51200/10/10, and the
// capture's mark/advance steps; the frozen commits are 0, 500 and 600 ms.
TEST_CASE("progress commits the first change at once then paces later commits by their written bytes",
        "[chord][output][progress][issue1187][spec]") {
    const std::vector<ProgressStep> steps{
            mark_step(),
            advance_step(0.0),
            mark_step(2),
            advance_step(499.0),
            advance_step(1.0),
            mark_step(),
            advance_step(99.0),
            advance_step(1.0),
    };
    const auto commits = run_progress_schedule(100ms, steps, {51'200, 10, 10});

    REQUIRE(commits.size() == 3);
    CHECK(kProgressBytesPerSecond == 102400);
    CHECK(commits[0].at.count() == 0.0);
    CHECK(commits[0].bytes == 51'200);
    CHECK(commits[1].at.count() == 500.0);
    CHECK(commits[1].bytes == 10);
    CHECK(commits[2].at.count() == 600.0);
    CHECK(commits[2].bytes == 10);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1187/chord-progress.json
// "progress keeps the exact fractional byte-rate pause instead of rounding it".
// The declared inputs are minIntervalMs 0 and writeBytes 1025/1; 1025 bytes
// buy exactly 10.009765625 ms, not a whole millisecond or a whole KiB.
TEST_CASE("progress keeps the exact fractional byte-rate pause instead of rounding it",
        "[chord][output][progress][issue1187][spec]") {
    const std::vector<ProgressStep> steps{
            mark_step(),
            advance_step(0.0),
            mark_step(),
            advance_step(10.009765625),
    };
    const auto commits = run_progress_schedule(0ms, steps, {1'025, 1});

    REQUIRE(commits.size() == 2);
    CHECK(commits[0].at.count() == 0.0);
    CHECK(commits[0].bytes == 1'025);
    CHECK(commits[1].at.count() == 10.009765625);
    CHECK(commits[1].bytes == 1);
}

// The discriminating cases separate the frozen pacing from the three plausible
// stand-ins: publishing per chunk (no throttle), always waiting one fixed
// millisecond interval, and rounding the written bytes or the pause. The
// frozen byte-rate constant under test is
// fixtures/pi-v1.3.0/audit-1057/json-output-support.json "the progress byte rate is the frozen
// 100 KiB/s constant".
TEST_CASE("progress pacing separates a byte-rate pause from a constant interval and rounded bytes",
        "[chord][output][progress][issue1187][discriminating]") {
    const auto frozen = json_output_scenario("the progress byte rate is the frozen 100 KiB/s constant");
    const auto bytes_per_second = frozen.number_member("bytesPerSecond");
    REQUIRE(bytes_per_second.has_value());
    const auto frozen_rate = static_cast<std::size_t>(*bytes_per_second);

    // The written bytes dominate the 100 ms minimum: 50 KiB buys 500 ms at the frozen rate.
    CHECK(progress_pause(51'200, 100ms).count() == 500.0);
    // A small commit waits the configured minimum instead of its 0.09765625 ms.
    CHECK(progress_pause(10, 100ms).count() == 100.0);
    // Rounding the bytes to whole KiBs or the pause to whole milliseconds both
    // read 10 ms here; the frozen fraction is kept exactly.
    CHECK(progress_pause(1'025, 0ms).count() == 10.009765625);
    CHECK(frozen_rate == 102400);

    ProgressPacer pacer{100ms};
    pacer.mark();
    CHECK(pacer.due(ProgressTime{}));
    pacer.begin();
    pacer.committed(ProgressTime{}, 51'200);
    CHECK_FALSE(pacer.pending());
    // A per-chunk pacer would be due immediately after this mark; the frozen
    // one waits until the written bytes have been paced.
    pacer.mark();
    CHECK_FALSE(pacer.due(ProgressTime{499.0}));
    CHECK(pacer.due(ProgressTime{500.0}));
}
