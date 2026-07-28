#include "cli/InitialPrompt.hpp"
#include "support/ImageFixture.hpp"
#include "support/TempWorkspace.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <string>
#include <vector>

using namespace cch;

TEST_CASE("Initial CLI files are content-sniffed and preserve text then image order", "[cli][initial-prompt][issue63]") {
    tests::TempWorkspace workspace;
    workspace.write(
        "one.data", tests::bytes_as_string(tests::decode_base64(tests::kTinyPngBase64)));
    workspace.write(
        "two.data", tests::bytes_as_string(tests::decode_base64(tests::kTinyJpegBase64)));
    workspace.write(
        "three.data", tests::bytes_as_string(tests::decode_base64(tests::kTinyGifBase64)));
    workspace.write(
        "four.data", tests::bytes_as_string(tests::decode_base64(tests::kTinyWebpBase64)));
    workspace.write("note.txt", "plain text");
    const std::vector<std::string> files{
        "one.data",
        "note.txt",
        "two.data",
        "three.data",
        "four.data",
    };

    const auto prepared = cli::prepare_initial_prompt("describe them", files, workspace.path());

    REQUIRE(prepared);
    REQUIRE(prepared->images.size() == 4);
    CHECK(prepared->images[0].mime_type == "image/png");
    CHECK(prepared->images[1].mime_type == "image/jpeg");
    CHECK(prepared->images[2].mime_type == "image/gif");
    CHECK(prepared->images[3].mime_type == "image/webp");
    const auto first = prepared->text.find("one.data\"></file>");
    const auto note = prepared->text.find("note.txt\">\nplain text\n</file>");
    const auto second = prepared->text.find("two.data\"></file>");
    const auto prompt = prepared->text.find("describe them");
    REQUIRE(first != std::string::npos);
    REQUIRE(note != std::string::npos);
    REQUIRE(second != std::string::npos);
    REQUIRE(prompt != std::string::npos);
    CHECK(first < note);
    CHECK(note < second);
    CHECK(second < prompt);
}

TEST_CASE("A lone initial image still produces prompt text", "[cli][initial-prompt][issue63]") {
    tests::TempWorkspace workspace;
    workspace.write(
        "only.png", tests::bytes_as_string(tests::decode_base64(tests::kTinyPngBase64)));
    const std::vector<std::string> files{"only.png"};

    const auto prepared = cli::prepare_initial_prompt({}, files, workspace.path());

    REQUIRE(prepared);
    REQUIRE(prepared->images.size() == 1);
    CHECK_FALSE(prepared->text.empty());
    CHECK(prepared->text.find("<file name=\"") != std::string::npos);
    CHECK(prepared->text.find("only.png\"></file>\n") != std::string::npos);
}
