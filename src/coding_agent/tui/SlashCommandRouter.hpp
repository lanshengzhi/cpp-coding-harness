#pragma once

#include <cch/support/Error.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <variant>

namespace cch::coding_agent::tui {

/// The canonical built-in slash command identities understood by the Native
/// TUI. Aliases (for example `/new` and `/scoped-models`) resolve to one of
/// these identities before routing.
enum class SlashCommandId {
    Clear,
    Quit,
    Copy,
    Session,
    Hotkeys,
    Settings,
    Help,
    Model,
    Models,
    Thinking,
    Login,
    Logout,
    Resume,
    Fork,
    Tree,
    Reload,
    Compact,
    Name,
    Trust,
};

/// The command and its already-trimmed argument. Arguments are optional for
/// commands whose pi-shaped handlers use the empty value as a distinct case,
/// such as `/model`, `/login`, `/name`, `/thinking`, and `/compact`.
struct SlashCommandInvocation {
    SlashCommandId command{SlashCommandId::Clear};
    std::string argument;
};

/// A non-slash submission, or an unrecognized slash submission explicitly
/// allowed through by the host for a dynamic prompt or skill command.
struct SlashCommandPassThrough {};

/// A parse or routing failure that the host can present without throwing or
/// converting the submission into an Agent Prompt.
struct SlashCommandRouteError {
    std::string message;
    /// True only when the command token itself was not a built-in. Hosts may
    /// use this to preserve dynamic prompt/skill commands while still
    /// rejecting malformed arguments for known built-ins.
    bool unknown_command{false};
};

/// Result of parsing one submission. Parsing is deliberately independent of
/// TUI rendering and immediate-command side effects.
using SlashCommandParseResult = std::variant<
    SlashCommandPassThrough,
    SlashCommandInvocation,
    SlashCommandRouteError>;

/// Result after a parsed command has been dispatched. Immediate commands have
/// already run against the supplied execution context; modal commands are
/// returned as structured requests for the owning interactive controller.
struct SlashCommandImmediateResult {
    SlashCommandInvocation invocation;
};

struct SlashCommandModalResult {
    SlashCommandInvocation invocation;
};

using SlashCommandRoute = std::variant<
    SlashCommandPassThrough,
    SlashCommandImmediateResult,
    SlashCommandModalResult,
    SlashCommandRouteError>;

/// The small host seam for synchronous, in-place command execution. The
/// router owns parsing, alias resolution, argument validation, and error
/// classification; the host owns effects such as updating the view or
/// requesting session replacement.
struct SlashCommandExecutionContext {
    std::move_only_function<support::ExpectedVoid(const SlashCommandInvocation&)>
        execute_immediate{nullptr};
    /// Optional predicate for dynamic slash resources (prompt templates and
    /// enabled `/skill:` commands). It receives the trimmed command token
    /// without the leading slash. A false or empty predicate makes unknown
    /// slash commands user-visible routing errors.
    std::move_only_function<bool(std::string_view)> allow_unrecognized{nullptr};
};

/// Return the canonical pi-shaped spelling for a command identity.
[[nodiscard]] std::string_view slash_command_name(SlashCommandId command) noexcept;

/// Whether a command is executed immediately by the host context rather than
/// returned as a modal request.
[[nodiscard]] bool is_immediate_slash_command(SlashCommandId command) noexcept;

/// Deep parser and router for Native TUI slash submissions. The module has no
/// dependency on InteractiveState, Terminal, or rendering: those concerns
/// enter only through SlashCommandExecutionContext and the returned modal
/// value.
class SlashCommandRouter final {
public:
    SlashCommandRouter() = default;
    SlashCommandRouter(SlashCommandRouter&&) = delete;
    SlashCommandRouter& operator=(SlashCommandRouter&&) = delete;
    ~SlashCommandRouter() = default;
    SlashCommandRouter(const SlashCommandRouter&) = delete;
    SlashCommandRouter& operator=(const SlashCommandRouter&) = delete;

    [[nodiscard]] static SlashCommandParseResult parse(std::string_view text);

    [[nodiscard]] SlashCommandRoute route(
        std::string_view text,
        SlashCommandExecutionContext& context) const;
};

} // namespace cch::coding_agent::tui
