// OSC 7501 program-status encoding and query-reply recognition (TStatus,
// #1123). Replays the registered frozen pi-v1.3.0 observations in
// fixtures/pi-v1.3.0/audit-1050/program-status-format.json, scenarios
// `format-program-status-encodes-fields-and-limits` and
// `program-status-reply-recognition` (revision 71023aec1546).

#include <cch/tui/ProgramStatus.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace {

/// Standard padded base64 decode for asserting the frozen `msg` field.
[[nodiscard]] std::string base64_decode(std::string_view encoded) {
    const auto value_of = [](char character) -> int {
        if (character >= 'A' && character <= 'Z') return character - 'A';
        if (character >= 'a' && character <= 'z') return character - 'a' + 26;
        if (character >= '0' && character <= '9') return character - '0' + 52;
        if (character == '+') return 62;
        if (character == '/') return 63;
        return -1;
    };
    std::string decoded;
    unsigned int accumulator = 0;
    int bits = 0;
    for (const auto character : encoded) {
        const auto value = value_of(character);
        if (value < 0) continue;
        accumulator = (accumulator << 6) | static_cast<unsigned int>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<char>((accumulator >> bits) & 0xff));
        }
    }
    return decoded;
}

[[nodiscard]] std::string decode_message(const std::string& sequence) {
    constexpr std::string_view kMarker = ":msg=";
    const auto marker = sequence.find(kMarker);
    if (marker == std::string::npos) return {};
    const auto begin = marker + kMarker.size();
    const auto end = sequence.find("\x1b\\", begin);
    return base64_decode(sequence.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
}

} // namespace

TEST_CASE("Program status format encodes frozen OSC 7501 fields and limits", "[tui][program-status][issue1123][spec]") {
    using cch::tui::format_program_status;
    using cch::tui::ProgramStatus;

    // state=blocked:app=pi:kind=permission with a base64 message.
    CHECK(format_program_status(ProgramStatus{
                  .state = ProgramStatus::State::Blocked,
                  .app = "pi",
                  .kind = ProgramStatus::Kind::Permission,
                  .message = "Allow bash?",
          }) == "\x1b]7501;state=blocked:app=pi:kind=permission:msg=QWxsb3cgYmFzaD8=\x1b\\");
    CHECK(format_program_status(ProgramStatus{.state = ProgramStatus::State::Clear}) == "\x1b]7501;state=clear\x1b\\");

    // An invalid app name, a kind outside blocked, and a blank message are all
    // omitted.
    CHECK(format_program_status(ProgramStatus{
                  .state = ProgramStatus::State::Working,
                  .app = "my app",
                  .kind = ProgramStatus::Kind::Auth,
                  .message = " \n ",
          }) == "\x1b]7501;state=working\x1b\\");
    const auto app_32 = std::string(32, 'a');
    const auto app_33 = std::string(33, 'a');
    CHECK(format_program_status(ProgramStatus{.state = ProgramStatus::State::Idle, .app = app_32})
                    .find(":app=" + app_32) != std::string::npos);
    CHECK(format_program_status(ProgramStatus{.state = ProgramStatus::State::Idle, .app = app_33}).find("app=") ==
            std::string::npos);

    // Control characters (C0, DEL, C1) become spaces so the terminal cannot
    // discard the report; runs collapse to one space and the text is trimmed.
    const auto control_characters = format_program_status(ProgramStatus{
            .state = ProgramStatus::State::Error,
            .message = "first\nsecond\x1b[31m\xc2\x9bthird\t",
    });
    CHECK(control_characters == "\x1b]7501;state=error:msg=Zmlyc3Qgc2Vjb25kIFszMW0gdGhpcmQ=\x1b\\");
    CHECK(decode_message(control_characters) == "first second [31m third");

    // A long message is cut at a UTF-8 boundary within the decoded 2048-byte
    // limit; the frozen `é`-repeat observation keeps 1024 characters and a
    // 2766-byte sequence.
    std::string long_message;
    for (std::size_t index = 0; index < 2000; ++index)
        long_message += "\xc3\xa9";
    const auto long_sequence = format_program_status(ProgramStatus{
            .state = ProgramStatus::State::Working,
            .app = "pi",
            .message = long_message,
    });
    auto expected_message = std::string{};
    for (std::size_t index = 0; index < 1024; ++index)
        expected_message += "\xc3\xa9";
    CHECK(decode_message(long_sequence) == expected_message);
    CHECK(decode_message(long_sequence).size() == 2048);
    CHECK(long_sequence.size() == 2766);
}

TEST_CASE("Program status reply recognition accepts only the frozen query echoes",
        "[tui][program-status][issue1123][spec]") {
    using cch::tui::is_program_status_reply;

    CHECK(is_program_status_reply("\x1b]7501;?\x1b\\"));
    CHECK(is_program_status_reply("\x1b]7501;?\x07"));
    CHECK(is_program_status_reply("\x1b]7501;?version=2\x1b\\"));
    CHECK_FALSE(is_program_status_reply("\x1b]7501;state=idle\x1b\\"));
    CHECK_FALSE(is_program_status_reply("\x1b]11;rgb:0000/0000/0000\x07"));

    // The terminator must end the sequence: a body carrying BEL/ESC or a
    // missing terminator is not a reply.
    CHECK_FALSE(is_program_status_reply("\x1b]7501;?"));
    CHECK_FALSE(is_program_status_reply("\x1b]7501;?a\x07b\x1b\\"));
    CHECK_FALSE(is_program_status_reply("\x1b]7501;?a\x1b\\b"));
}
