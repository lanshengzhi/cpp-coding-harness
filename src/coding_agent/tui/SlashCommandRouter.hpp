#pragma once

#include <cch/support/Error.hpp>

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace cch::coding_agent {
struct PromptTemplate;
struct Skill;
} // namespace cch::coding_agent

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
    Mcp,
};

/// The sub-command of a two-level slash command. `/mcp` is the only one: it
/// is the read-only Upstream Connection Status overview with no argument, and
/// `/mcp auth <server>` is the browser authorization behind it (issue #849).
/// Parsing it here rather than in the host is what keeps the command's shape
/// testable without a Terminal, and what makes `/mcp nonsense` a visible
/// error rather than a silent overview.
enum class SlashCommandSub { None, McpAuth };

/// The command and its already-trimmed argument. Arguments are optional for
/// commands whose pi-shaped handlers use the empty value as a distinct case,
/// such as `/model`, `/login`, `/name`, `/thinking`, and `/compact`.
struct SlashCommandInvocation {
    SlashCommandId command{SlashCommandId::Clear};
    std::string argument;
    /// The sub-command, for a two-level command. `None` is the whole of a
    /// one-level command's shape, and the overview case of `/mcp`.
    SlashCommandSub sub{SlashCommandSub::None};
    /// The sub-command's own argument — the Server Id of `/mcp auth <server>`.
    /// Empty exactly when `sub` is `None`.
    std::string sub_argument;
};

/// The one Server Id grammar a `/mcp auth <server>` argument must satisfy:
/// the `CONTEXT.md` `[A-Za-z0-9_-]` set. A Server Id that could contain
/// whitespace or a credential-key separator would make the sub-argument
/// ambiguous, so one that does is a visible error at parse time rather than a
/// lookup that finds nothing.
[[nodiscard]] bool is_valid_mcp_server_id(std::string_view server_id) noexcept;

/// A non-slash submission, or an unrecognized slash submission: pi's Native
/// TUI dispatches only its built-in names and hands every other submission to
/// `session.prompt`, so unrecognized slash text is an ordinary Agent Prompt.
struct SlashCommandPassThrough {};

/// The reason a slash submission could not be routed.
enum class SlashCommandRouteErrorKind {
    Invalid,
    UnknownCommand,
};

/// A parse or routing failure that the host can present without throwing or
/// converting the submission into an Agent Prompt.
struct SlashCommandRouteError {
    std::string message;
    /// UnknownCommand marks a command token that names no built-in. Parsing
    /// reports it; routing passes the submission through as an Agent Prompt
    /// (pi parity, issue #792).
    SlashCommandRouteErrorKind kind{SlashCommandRouteErrorKind::Invalid};
};

/// Result of parsing one submission. Parsing is deliberately independent of
/// TUI rendering and immediate-command side effects.
using SlashCommandParseResultVariant = std::variant<
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

using SlashCommandRouteVariant = std::variant<
    SlashCommandPassThrough,
    SlashCommandImmediateResult,
    SlashCommandModalResult,
    SlashCommandRouteError>;

/// The small host seam for synchronous, in-place command execution. The
/// router owns parsing, alias resolution, argument validation, and error
/// classification; the host owns effects such as updating the view or
/// requesting session replacement.
struct SlashCommandExecutionContext {
    std::move_only_function<support::ExpectedVoid(const SlashCommandInvocation&)> execute_immediate{nullptr};
};

/// Return the canonical pi-shaped spelling for a command identity.
[[nodiscard]] std::string_view slash_command_name(SlashCommandId command) noexcept;

/// One accepted built-in slash spelling and the identity it resolves to.
struct SlashCommandSpelling {
    std::string_view spelling;
    SlashCommandId command;
};

/// Every accepted built-in slash spelling, canonical names and aliases alike.
/// The command palette offers all of them so an exact spelling cannot lose to
/// an unrelated fuzzy match (issue #791).
[[nodiscard]] std::span<const SlashCommandSpelling> slash_command_spellings() noexcept;

/// Whether a command is executed immediately by the host context rather than
/// returned as a modal request.
[[nodiscard]] bool is_immediate_slash_command(SlashCommandId command) noexcept;

/// Deep parser and router for Native TUI slash submissions. The module has no
/// dependency on the InteractiveEngine, Terminal, or rendering: those concerns
/// enter only through SlashCommandExecutionContext and the returned modal
/// value.
class SlashCommandRouter final {
public:
    [[nodiscard]] static SlashCommandParseResultVariant parse(std::string_view text);

    [[nodiscard]] SlashCommandRouteVariant route(
        std::string_view text,
        SlashCommandExecutionContext& context) const;
};

} // namespace cch::coding_agent::tui
