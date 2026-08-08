#include "cli/InitialPrompt.hpp"
#include "support/ImageFixture.hpp"
#include "support/TempWorkspace.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <string>
#include <vector>

using namespace cch;

namespace {

cli::InitialMessageInput input_with(
    const std::filesystem::path& working_directory,
    std::vector<std::string> messages = {},
    std::vector<std::string> file_arguments = {},
    std::string stdin_content = {}) {
    return cli::InitialMessageInput{
        .messages = std::move(messages),
        .file_arguments = std::move(file_arguments),
        .working_directory = working_directory,
        .stdin_content = std::move(stdin_content),
    };
}

} // namespace

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

    const auto prepared = cli::build_initial_message(
        input_with(workspace.path(), {"describe them"}, files));

    REQUIRE(prepared);
    REQUIRE(prepared->initial_images.size() == 4);
    CHECK(prepared->initial_images[0].mime_type == "image/png");
    CHECK(prepared->initial_images[1].mime_type == "image/jpeg");
    CHECK(prepared->initial_images[2].mime_type == "image/gif");
    CHECK(prepared->initial_images[3].mime_type == "image/webp");
    const auto first = prepared->initial_message.find("one.data\"></file>");
    const auto note = prepared->initial_message.find("note.txt\">\nplain text\n</file>");
    const auto second = prepared->initial_message.find("two.data\"></file>");
    const auto prompt = prepared->initial_message.find("describe them");
    REQUIRE(first != std::string::npos);
    REQUIRE(note != std::string::npos);
    REQUIRE(second != std::string::npos);
    REQUIRE(prompt != std::string::npos);
    CHECK(first < note);
    CHECK(note < second);
    CHECK(second < prompt);
    CHECK(prepared->remaining_messages.empty());
}

TEST_CASE("A lone initial image still produces initial message text", "[cli][initial-prompt][issue63]") {
    tests::TempWorkspace workspace;
    workspace.write(
        "only.png", tests::bytes_as_string(tests::decode_base64(tests::kTinyPngBase64)));
    const std::vector<std::string> files{"only.png"};

    const auto prepared = cli::build_initial_message(input_with(workspace.path(), {}, files));

    REQUIRE(prepared);
    REQUIRE(prepared->initial_images.size() == 1);
    CHECK_FALSE(prepared->initial_message.empty());
    CHECK(prepared->initial_message.find("<file name=\"") != std::string::npos);
    CHECK(prepared->initial_message.find("only.png\"></file>\n") != std::string::npos);
}

TEST_CASE("Initial message merges piped stdin with the first CLI message into one prompt", "[cli][initial-prompt]") {
    tests::TempWorkspace workspace;

    const auto prepared = cli::build_initial_message(
        input_with(workspace.path(), {"Summarize the text given"}, {}, "README contents\n"));

    REQUIRE(prepared);
    CHECK(prepared->initial_message == "README contents\nSummarize the text given");
    CHECK(prepared->remaining_messages.empty());
}

TEST_CASE("Initial message uses piped stdin as the prompt when no CLI message is present", "[cli][initial-prompt]") {
    tests::TempWorkspace workspace;

    const auto prepared = cli::build_initial_message(
        input_with(workspace.path(), {}, {}, "README contents"));

    REQUIRE(prepared);
    CHECK(prepared->initial_message == "README contents");
    CHECK(prepared->remaining_messages.empty());
}

TEST_CASE("Initial message combines stdin, file text, and the first CLI message in pi's order", "[cli][initial-prompt]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "file text");

    const auto prepared = cli::build_initial_message(
        input_with(workspace.path(), {"Explain it", "Second message"}, {"note.txt"}, "stdin\n"));

    REQUIRE(prepared);
    // pi `buildInitialMessage`: stdin, then @file text, then the first
    // message, joined with no separator; the rest prompt sequentially.
    CHECK(prepared->initial_message == "stdin\n<file name=\"" +
        (workspace.path() / "note.txt").string() + "\">\nfile text\n</file>\nExplain it");
    REQUIRE(prepared->remaining_messages.size() == 1);
    CHECK(prepared->remaining_messages[0] == "Second message");
}

TEST_CASE("Initial message omits absent stdin and message parts", "[cli][initial-prompt]") {
    tests::TempWorkspace workspace;

    const auto none = cli::build_initial_message(input_with(workspace.path()));
    REQUIRE(none);
    CHECK(none->initial_message.empty());
    CHECK(none->remaining_messages.empty());

    const auto only_message = cli::build_initial_message(
        input_with(workspace.path(), {"hello", "world"}));
    REQUIRE(only_message);
    CHECK(only_message->initial_message == "hello");
    REQUIRE(only_message->remaining_messages.size() == 1);
    CHECK(only_message->remaining_messages[0] == "world");
}

TEST_CASE("Initial message merge preserves trailing stdin newlines exactly", "[cli][initial-prompt]") {
    tests::TempWorkspace workspace;

    const auto prepared = cli::build_initial_message(
        input_with(workspace.path(), {"Summarize"}, {}, "line one\nline two\n"));

    REQUIRE(prepared);
    CHECK(prepared->initial_message == "line one\nline two\nSummarize");
}
