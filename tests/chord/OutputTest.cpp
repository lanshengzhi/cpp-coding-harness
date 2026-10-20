#include <cch/chord/Output.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <iterator>
#include <string>

using namespace cch;
using namespace cch::chord;

namespace {

const std::string kMixed = "a\xc3\xa9\xf0\x9f\x98\x80"; // "aé😀"

} // namespace

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/json-output-support.json
// "UTF-8 byte length and character-end boundaries match the frozen output helpers".
TEST_CASE("UTF-8 byte length and character-end boundaries match the frozen output helpers",
        "[chord][output][utf8][issue1060][spec]") {
    CHECK(utf8_byte_length("abc") == 3);
    CHECK(utf8_byte_length("\xc3\xa9") == 2);
    CHECK(utf8_byte_length("\xf0\x9f\x98\x80") == 4);
    CHECK(utf8_byte_length(kMixed) == 7);

    const std::size_t boundaries[] = {0, 1, 1, 3, 3, 3, 3, 7};
    for (std::size_t index = 0; index < std::size(boundaries); ++index) {
        CHECK(character_end(kMixed, index) == boundaries[index]);
    }
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/json-output-support.json
// "boundOutput and OutputBuffer retain the configured head or tail and report exact dropped counts".
TEST_CASE("bound output keeps the configured head or tail and reports exact dropped counts",
        "[chord][output][bound][issue1060][spec]") {
    const auto head = bound_output("abcdefghij", OutputLimits{4, 1, OutputRetention::Head});
    CHECK(head.text == "abcd");
    CHECK(head.bytes == 4);
    CHECK(head.dropped_bytes == 6);
    CHECK(head.dropped_lines == 0);

    const auto tail = bound_output("abcdefghij", OutputLimits{4, 1, OutputRetention::Tail});
    CHECK(tail.text == "ghij");
    CHECK(tail.bytes == 4);
    CHECK(tail.dropped_bytes == 6);
    CHECK(tail.dropped_lines == 0);

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

TEST_CASE("a multi-byte character is cut only on a character boundary", "[chord][output][utf8][issue1060][spec]") {
    const auto head_two = bound_output(kMixed, OutputLimits{2, 1, OutputRetention::Head});
    CHECK(head_two.text == "a");
    CHECK(head_two.bytes == 1);
    CHECK(head_two.dropped_bytes == 6);

    const auto head_three = bound_output(kMixed, OutputLimits{3, 1, OutputRetention::Head});
    CHECK(head_three.text == "a\xc3\xa9");
    CHECK(head_three.bytes == 3);
    CHECK(head_three.dropped_bytes == 4);

    const auto tail_four = bound_output(kMixed, OutputLimits{4, 1, OutputRetention::Tail});
    CHECK(tail_four.text == "\xf0\x9f\x98\x80");
    CHECK(tail_four.bytes == 4);
    CHECK(tail_four.dropped_bytes == 3);

    const auto tail_two = bound_output(kMixed, OutputLimits{2, 1, OutputRetention::Tail});
    CHECK(tail_two.text.empty());
    CHECK(tail_two.bytes == 0);
    CHECK(tail_two.dropped_bytes == 7);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/json-output-support.json
// "boundOutput and OutputBuffer retain the configured head or tail and report exact dropped counts"
// (pushes "abc", "def", "gh"; snapshot text "efgh").
TEST_CASE("output buffer retains the frozen tail window and exact whole-stream totals",
        "[chord][output][buffer][issue1060][spec]") {
    OutputBuffer buffer(OutputLimits{4, 1, OutputRetention::Tail});

    CHECK(buffer.push("abc"));
    CHECK(buffer.push("def"));
    CHECK(buffer.push("gh"));

    const auto snapshot = buffer.snapshot();
    CHECK(snapshot.text == "efgh");
    CHECK(snapshot.dropped_bytes == 4);
    CHECK(snapshot.dropped_lines == 0);
    CHECK_FALSE(buffer.push(""));
}

TEST_CASE("output buffer keeps whole-stream dropped line counts across a tail window",
        "[chord][output][buffer][issue1060][spec]") {
    OutputBuffer buffer(OutputLimits{5, 1, OutputRetention::Tail});

    CHECK(buffer.push("one\n"));
    CHECK(buffer.push("two\n"));
    CHECK(buffer.push("three"));

    const auto snapshot = buffer.snapshot();
    CHECK(snapshot.text == "three");
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
