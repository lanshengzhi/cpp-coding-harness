// The oracles the TUI screen cases use when a workspace or session path is
// wider than the frame (issue #1232 adjacent finding). They are asserted here
// with literal rows so a screen case cannot pass because the oracle degraded
// into "anything matches": each case names a rendering the assertion must
// accept and one it must reject.

#include "support/FrameText.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace cch;

TEST_CASE("truncated_row keeps the leading cells and the ellipsis inside the width",
        "[support][frame-text][issue1232][spec]") {
    // pi footer.ts: `truncateToWidth(pwd, width, "...")`.
    CHECK(tests::truncated_row("/srv/app", 40) == "/srv/app");
    CHECK(tests::truncated_row("/srv/application-with-a-name-that-does-not-fit", 20) == "/srv/application-...");
    CHECK(tests::truncated_row("/srv/application-with-a-name-that-does-not-fit", 20).size() == 20);
    CHECK(tests::truncated_row_prefix("/srv/application-with-a-name-that-does-not-fit", 20) == "/srv/application-");
    // A row that fits keeps no ellipsis, which is what separates truncation
    // from a wrap.
    CHECK(tests::truncated_row("/srv/app-that-fits", 20) == "/srv/app-that-fits");
}

TEST_CASE("wrapped_rows splits a space-free path at the frame width", "[support][frame-text][issue1232][spec]") {
    const std::string path = "/var/tmp/cpp-harness-test-1750000000000000000/vanished";
    const auto rows = tests::wrapped_rows(path, 20);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0] == "/var/tmp/cpp-harness");
    CHECK(rows[1] == "-test-17500000000000");
    CHECK(rows[2] == "00000/vanished");
    CHECK(rows[0] + rows[1] + rows[2] == path);
}

TEST_CASE("rows_show_text tiles a path across the rows a wrapping Text renders",
        "[support][frame-text][issue1232][spec]") {
    // A `Text` with padding_x 1 at width 25: content width 23, one indent
    // column on every wrapped row, and each row padded to the frame.
    const std::string path = "/var/tmp/cpp-harness-test-1750000000000000000";
    const std::vector<std::string> rows{
            " Session cwd not found   ",
            " /var/tmp/cpp-harness-te ",
            " st-1750000000000000000  ",
            " continue in current cwd ",
    };
    CHECK(tests::rows_show_text(rows, 25, path));
    // A needle that sits inside one row still works.
    CHECK(tests::rows_show_text(rows, 25, "Session cwd not found"));
    // A truncated tail fails: the second segment is what a renderer that
    // abbreviates the path would drop.
    CHECK_FALSE(tests::rows_show_text(rows, 25, path + "/vanished"));
    // A missing middle segment fails as well, not just a missing tail.
    CHECK_FALSE(tests::rows_show_text(rows, 25, "/var/tmp/cpp-harness-000000000000000000"));
}

TEST_CASE("rows_show_text does not assemble a needle from two unrelated short rows",
        "[support][frame-text][issue1232][spec]") {
    // Neither row filled the frame, so the renderer never broke a text there:
    // the two rows are separate lines and must not read as one.
    const std::vector<std::string> rows{
            "abc                              ",
            "def                              ",
    };
    CHECK_FALSE(tests::rows_show_text(rows, 33, "abcdef"));
    CHECK(tests::rows_show_text(rows, 33, "abc"));
}
