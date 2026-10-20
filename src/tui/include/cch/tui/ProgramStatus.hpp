#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace cch::tui {

/// What a program tells the terminal it is doing (OSC 7501, pi
/// `packages/tui/src/program-status.ts` `ProgramStatus`). Only the root record
/// is supported; `Clear` removes the reported status instead of reporting one.
struct ProgramStatus {
    enum class State {
        Idle,
        Working,
        Blocked,
        Done,
        Error,
        Clear,
    };

    /// What a blocked program waits for; omitted for other states.
    enum class Kind {
        Permission,
        Question,
        Auth,
    };

    State state{State::Idle};
    /// Stable program name, `[A-Za-z0-9_.+-]{1,32}`. Other values are omitted.
    std::optional<std::string> app{};
    std::optional<Kind> kind{};
    /// One human-readable line. Control characters become spaces; longer text
    /// is cut to the protocol's 2048-byte decoded limit.
    std::optional<std::string> message{};

    bool operator==(const ProgramStatus&) const = default;
};

/// Feature detection query (pi `PROGRAM_STATUS_QUERY`). A supporting terminal
/// replies with the same body, terminated by BEL or ST.
inline constexpr std::string_view kProgramStatusQuery = "\x1b]7501;?\x1b\\";

/// Encode one status report (pi `formatProgramStatus`). Control characters are
/// replaced because terminals discard reports whose text contains them; the
/// message is base64-encoded so it stays one protocol field.
[[nodiscard]] std::string format_program_status(const ProgramStatus& status);

/// Whether `sequence` is the reply to `kProgramStatusQuery` (pi
/// `isProgramStatusReply`): the query echo with either terminator and optional
/// future pairs after the `?`.
[[nodiscard]] bool is_program_status_reply(std::string_view sequence);

} // namespace cch::tui
