#include "SessionFamily.hpp"

#include "coding_agent/SessionDiscovery.hpp"
#include "coding_agent/SessionPathPolicy.hpp"
#include <cch/coding_agent/AgentConfigDir.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cch::cli {
namespace {

[[nodiscard]] std::vector<std::string_view> present_flags(
    std::initializer_list<std::pair<bool, std::string_view>> candidates) {
    std::vector<std::string_view> present;
    for (const auto& [engaged, flag] : candidates) {
        if (engaged) {
            present.push_back(flag);
        }
    }
    return present;
}

[[nodiscard]] std::string join_flags(
    const std::vector<std::string_view>& flags) {
    std::string joined;
    for (std::size_t index = 0; index < flags.size(); ++index) {
        if (index > 0) {
            joined += ", ";
        }
        joined += flags[index];
    }
    return joined;
}

/// pi `resolveSessionPath`-consuming if-chain state: the effective session
/// directory (custom override or the workspace-keyed default), the engaged
/// cwd filter (pi `SessionManager.list` filter semantics: a custom override
/// that differs from the default filters by header cwd), and the custom
/// directory for the global search (pi `listAll`).
struct SessionSearchSpace {
    std::filesystem::path local_directory;
    std::optional<std::filesystem::path> cwd_filter;
    std::optional<std::filesystem::path> custom_directory;
};

[[nodiscard]] SessionSearchSpace make_search_space(
    const std::filesystem::path& workspace,
    const std::optional<std::filesystem::path>& effective_directory) {
    const auto sessions_root = coding_agent::sessions_root_path();
    const auto default_directory =
        sessions_root / coding_agent::session_paths::encode_workspace_key(workspace);
    SessionSearchSpace space;
    space.local_directory = effective_directory.value_or(default_directory);
    if (effective_directory && *effective_directory != default_directory) {
        space.cwd_filter = workspace;
        space.custom_directory = effective_directory;
    }
    return space;
}

[[nodiscard]] coding_agent::session_discovery::ResolvedSessionArg resolve_session_arg(
    const std::string& arg,
    const std::filesystem::path& workspace,
    const SessionSearchSpace& space) {
    return coding_agent::session_discovery::resolve_session_arg(
        arg,
        workspace,
        space.local_directory,
        space.cwd_filter,
        coding_agent::sessions_root_path(),
        space.custom_directory);
}

} // namespace

std::optional<std::string> session_family_guard_error(const CliConfig& config) {
    // pi validateForkFlags: --fork cannot be combined with --session,
    // --continue, --resume, or --no-session.
    if (config.fork) {
        const auto conflicts = present_flags({
            {config.session_value.has_value(), "--session"},
            {config.continue_session, "--continue"},
            {config.resume_value.has_value(), "--resume"},
            {config.no_session_flag, "--no-session"},
        });
        if (!conflicts.empty()) {
            return "Error: --fork cannot be combined with " +
                   join_flags(conflicts);
        }
    }

    // pi validateSessionIdFlags: --session-id cannot be combined with
    // --session, --continue, or --resume (--fork and --no-session stay
    // allowed), then assertValidSessionId.
    if (config.session_id) {
        const auto conflicts = present_flags({
            {config.session_value.has_value(), "--session"},
            {config.continue_session, "--continue"},
            {config.resume_value.has_value(), "--resume"},
        });
        if (!conflicts.empty()) {
            return "Error: --session-id cannot be combined with " +
                   join_flags(conflicts);
        }
        if (auto invalid =
                coding_agent::session_discovery::invalid_session_id_reason(*config.session_id);
            invalid) {
            return "Error: " + *invalid;
        }
    }

    return std::nullopt;
}

std::optional<std::string> session_name_guard_error(const CliConfig& config) {
    // pi main.ts: `--name` requires a non-empty trimmed value.
    if (config.name) {
        const auto not_space = [](unsigned char character) {
            return !std::isspace(character);
        };
        std::string trimmed = *config.name;
        trimmed.erase(
            trimmed.begin(),
            std::find_if(trimmed.begin(), trimmed.end(), not_space));
        trimmed.erase(
            std::find_if(trimmed.rbegin(), trimmed.rend(), not_space).base(),
            trimmed.end());
        if (trimmed.empty()) {
            return "Error: --name requires a non-empty value";
        }
    }
    return std::nullopt;
}

util::Expected<SessionFamilyAssembly> assemble_session_target(
    const CliConfig& config,
    const std::optional<std::string>& settings_session_dir,
    std::istream& input,
    std::ostream& output,
    std::ostream& error) {
    // pi main.ts sessionDir chain: --session-dir, then
    // PI_CODING_AGENT_SESSION_DIR, then the settings sessionDir value.
    std::optional<std::string> env_value;
    if (const char* env = std::getenv("PI_CODING_AGENT_SESSION_DIR");
        env != nullptr && env[0] != '\0') {
        env_value = std::string{env};
    }
    auto effective = coding_agent::session_paths::resolve_effective_session_dir(
        config.session_dir,
        env_value,
        settings_session_dir,
        config.workspace,
        coding_agent::home_directory());
    if (!effective) {
        return std::unexpected(effective.error());
    }
    const auto space = make_search_space(config.workspace, *effective);

    // pi createSessionManager first check: --no-session (and the help /
    // --list-models print paths) short-circuit to an in-memory session,
    // silently winning over --session/--resume/--continue/--session-id.
    if (config.no_session_flag || config.list_models) {
        return SessionFamilyAssembly{
            .target = coding_agent::InMemorySessionTarget{config.session_id},
            .aborted = false,
        };
    }

    if (config.fork) {
        // pi: the --fork + --session-id target id must not collide with an
        // existing local session (local-only check, no cross-project prompt).
        if (config.session_id) {
            const auto local = coding_agent::session_discovery::list_sessions_in_directory(
                space.local_directory, space.cwd_filter);
            const auto occupied = std::find_if(
                local.begin(), local.end(),
                [&](const coding_agent::session_discovery::SessionInfoLite& session) {
                    return session.id == *config.session_id;
                });
            if (occupied != local.end()) {
                return std::unexpected(util::make_error(
                    util::ErrorCode::Session,
                    "Session already exists with id '" + *config.session_id + "'"));
            }
        }

        const auto resolved = resolve_session_arg(*config.fork, config.workspace, space);
        switch (resolved.kind) {
        case coding_agent::session_discovery::SessionArgKind::Path:
        case coding_agent::session_discovery::SessionArgKind::Local:
        case coding_agent::session_discovery::SessionArgKind::Global:
            return SessionFamilyAssembly{
                .target = coding_agent::ForkSessionTarget{
                    .source_path = std::move(resolved.path),
                    .session_id = config.session_id,
                },
                .aborted = false,
            };
        case coding_agent::session_discovery::SessionArgKind::NotFound:
            return std::unexpected(util::make_error(
                util::ErrorCode::Session,
                "No session found matching '" + resolved.arg + "'"));
        }
    }

    if (config.session_value.has_value()) {
        const auto resolved = resolve_session_arg(
            *config.session_value, config.workspace, space);
        switch (resolved.kind) {
        case coding_agent::session_discovery::SessionArgKind::Path:
        case coding_agent::session_discovery::SessionArgKind::Local:
            return SessionFamilyAssembly{
                .target = coding_agent::ExplicitOpenOrCreateSessionTarget{
                    .path = std::move(resolved.path),
                },
                .aborted = false,
            };
        case coding_agent::session_discovery::SessionArgKind::Global: {
            // pi: a session in another project prints its cwd and prompts to
            // fork into the current directory; declining aborts with exit 0.
            // The answer matches pi's promptConfirm exactly: lowercased line
            // equal to "y" or "yes" (no trimming).
            output << "Session found in different project: " << resolved.cwd
                   << '\n';
            output << "Fork this session into current directory? [y/N] "
                   << std::flush;
            std::string answer;
            std::getline(input, answer);
            for (char& character : answer) {
                if (character >= 'A' && character <= 'Z') {
                    character = static_cast<char>(character - 'A' + 'a');
                }
            }
            if (answer == "y" || answer == "yes") {
                return SessionFamilyAssembly{
                    .target = coding_agent::ForkSessionTarget{
                        .source_path = std::move(resolved.path),
                        .session_id = std::nullopt,
                    },
                    .aborted = false,
                };
            }
            output << "Aborted.\n";
            return SessionFamilyAssembly{
                .target = {},
                .aborted = true,
            };
        }
        case coding_agent::session_discovery::SessionArgKind::NotFound:
            return std::unexpected(util::make_error(
                util::ErrorCode::Session,
                "No session found matching '" + resolved.arg + "'"));
        }
    }

    if (config.resume_value.has_value()) {
        return SessionFamilyAssembly{
            .target = coding_agent::ExplicitResumeSessionTarget{
                .path = *config.resume_value,
            },
            .aborted = false,
        };
    }

    if (config.continue_session) {
        return SessionFamilyAssembly{
            .target = coding_agent::ContinueRecentSessionTarget{},
            .aborted = false,
        };
    }

    if (config.session_id) {
        // pi: an existing local session with the exact id resumes; otherwise
        // the warn-create message precedes creating a session with that id.
        const auto local = coding_agent::session_discovery::list_sessions_in_directory(
            space.local_directory, space.cwd_filter);
        const auto match = std::find_if(
            local.begin(), local.end(),
            [&](const coding_agent::session_discovery::SessionInfoLite& session) {
                return session.id == *config.session_id;
            });
        if (match != local.end()) {
            return SessionFamilyAssembly{
                .target = coding_agent::ExplicitOpenOrCreateSessionTarget{
                    .path = match->path,
                },
                .aborted = false,
            };
        }
        error << "Warning: No project session found with id '"
              << *config.session_id
              << "'; creating a new session with that id.\n";
        return SessionFamilyAssembly{
            .target = coding_agent::DefaultPersistedSessionTarget{
                .session_id = config.session_id,
            },
            .aborted = false,
        };
    }

    return SessionFamilyAssembly{
        .target = coding_agent::DefaultPersistedSessionTarget{std::nullopt},
        .aborted = false,
    };
}

} // namespace cch::cli
