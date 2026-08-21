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
};

/// The command and its already-trimmed argument. Arguments are optional for
/// commands whose pi-shaped handlers use the empty value as a distinct case,
/// such as `/model`, `/login`, `/name`, `/thinking`, and `/compact`.
struct SlashCommandInvocation {
    SlashCommandId command{SlashCommandId::Clear};
    std::string argument;
};

/// A non-slash submission, or an unrecognized slash submission explicitly
/// allowed through by the host for a dynamic resource or compatible
/// absolute-path prompt.
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
    /// UnknownCommand is set only when the command token itself was not a
    /// built-in. Hosts may use it to preserve dynamic prompt/skill commands
    /// while still rejecting malformed arguments for known built-ins.
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
    std::move_only_function<support::ExpectedVoid(const SlashCommandInvocation&)>
        execute_immediate{nullptr};
    /// Optional predicate for host-recognized dynamic slash resources
    /// (prompt templates and enabled `/skill:` commands) or compatible
    /// absolute-path prompt submissions. It receives the trimmed command
    /// token without the leading slash. A false or empty predicate makes
    /// unknown slash commands user-visible routing errors.
    std::move_only_function<bool(std::string_view)> allow_unrecognized{nullptr};
};

/// Return the canonical pi-shaped spelling for a command identity.
[[nodiscard]] std::string_view slash_command_name(SlashCommandId command) noexcept;

/// Whether a command is executed immediately by the host context rather than
/// returned as a modal request.
[[nodiscard]] bool is_immediate_slash_command(SlashCommandId command) noexcept;

/// Whether one trimmed command token (without the leading slash) names a
/// host-recognized dynamic slash resource: a loaded prompt template, or a
/// `/skill:` command over an enabled skill (pi's dynamic command surface).
/// Skill commands count only while the `enableSkillCommands` setting is
/// enabled.
[[nodiscard]] bool is_dynamic_slash_command(
    std::string_view command,
    std::span<const coding_agent::PromptTemplate> prompt_templates,
    std::span<const coding_agent::Skill> skills,
    bool skill_commands_enabled);

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
