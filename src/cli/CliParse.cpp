#include "CliParse.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cch::cli {
namespace {

#ifndef CCH_PROJECT_VERSION
#define CCH_PROJECT_VERSION "0.0.0"
#endif

constexpr std::string_view kProjectVersion = CCH_PROJECT_VERSION;

[[nodiscard]] cch::support::Error cli_error(std::string message) {
    return cch::support::make_error(cch::support::ErrorCode::Validation, message, message);
}

/// pi's short spellings, matched exactly against whole argv tokens (pi args.ts
/// hand parser matches the full token before any short-flag bundling). The
/// multi-character shorts (-na/-ns/-np/-nc) are normalized to their long
/// spellings before the manual parser handles the option table.
[[nodiscard]] std::string expand_pi_short(std::string_view token) {
    if (token == "-p") return "--print";
    if (token == "-a") return "--approve";
    if (token == "-na") return "--no-approve";
    if (token == "-r") return "--resume";
    if (token == "-c") return "--continue";
    if (token == "-n") return "--name";
    if (token == "-ns") return "--no-skills";
    if (token == "-np") return "--no-prompt-templates";
    if (token == "-nc") return "--no-context-files";
    if (token == "-h") return "--help";
    if (token == "-v") return "--version";
    return std::string{token};
}

/// pi `--list-models [search]`: the token after the flag is the fuzzy search
/// pattern only when it is not a flag and not an `@file` argument (pi args.ts
/// exact-match semantics). The intent is extracted before the option parser;
/// a bare flag records an empty search.
struct NormalizedArgv {
    std::vector<std::string> tokens;
    /// has_value() when --list-models appeared; empty string is the bare flag.
    std::optional<std::string> list_models;
};

[[nodiscard]] NormalizedArgv normalize_argv(
    const std::vector<std::string>& raw) {
    NormalizedArgv normalized;
    normalized.tokens.reserve(raw.size());
    bool options_ended = false;
    for (std::size_t index = 0; index < raw.size(); ++index) {
        const auto& token = raw[index];
        if (options_ended) {
            // Tokens after `--` are positionals verbatim (no short expansion,
            // no list-models extraction).
            normalized.tokens.push_back(token);
            continue;
        }
        if (token == "--") {
            options_ended = true;
            normalized.tokens.push_back(token);
            continue;
        }
        if (token == "--list-models" || token.starts_with("--list-models=")) {
            std::string search;
            if (const auto equals = token.find('='); equals != std::string::npos) {
                search = token.substr(equals + 1);
            } else if (index + 1 < raw.size() &&
                       !raw[index + 1].starts_with('-') &&
                       !raw[index + 1].starts_with('@')) {
                search = raw[++index];
            }
            normalized.list_models = std::move(search);
            continue;
        }
        normalized.tokens.push_back(expand_pi_short(token));
    }
    return normalized;
}

[[nodiscard]] cch::support::ExpectedVoid validate_output_mode(
    const std::string& mode_text) {
    // The internal OutputMode reduces to the text default (pi): only
    // `--mode text` parses; the removed json/rpc values are hard-rejected so
    // no removed surface is ever accepted-but-ignored.
    if (mode_text == "text") {
        return {};
    }
    if (mode_text == "json" || mode_text == "rpc") {
        return std::unexpected(cli_error(
            "--mode " + mode_text + " was removed; only --mode text is supported"));
    }
    return std::unexpected(cli_error("unsupported --mode: " + mode_text));
}

/// The pi-aligned option surface documented in help: pi's spellings and shorts
/// for every kept flag, with the C++ binary's own identity.
[[nodiscard]] std::string help_text() {
    return std::string{"pike - coding-agent Runtime with read, bash, edit, write tools\n"
                       "\n"
                       "Usage:\n"
                       "  pike [options] [@files...] [messages...]\n"
                       "\n"
                       "Options:\n"
                       "  --provider <name>              Provider name\n"
                       "  --model <pattern>              Model pattern or ID (supports \"provider/id\"\n"
                       "                                 and optional \":<thinking>\")\n"
                       "  --models <patterns>            Comma-separated model patterns for cycling\n"
                       "  --api-key <key>                API key (in-memory runtime override, never\n"
                       "                                 persisted; requires an explicit model)\n"
                       "  --thinking <level>             Set thinking level: off, minimal, low, medium,\n"
                       "                                 high, xhigh, max\n"
                       "  --print, -p                    Non-interactive mode: process prompt and exit\n"
                       "  --mode <mode>                  Output mode: text (default)\n"
                       "  --continue, -c                 Continue previous session\n"
                       "  --resume, -r                   Select a session to resume\n"
                       "  --session <path|id>            Use specific session file or partial UUID\n"
                       "  --session-id <id>              Use exact project session ID, creating it if\n"
                       "                                 missing\n"
                       "  --fork <path|id>               Fork specific session file or partial UUID\n"
                       "                                 into a new session\n"
                       "  --session-dir <dir>            Directory for session storage and lookup\n"
                       "  --no-session                   Don't save session (ephemeral)\n"
                       "  --name, -n <name>              Set session display name\n"
                       "  --approve, -a                  Trust project-local files for this run\n"
                       "  --no-approve, -na              Ignore project-local files for this run\n"
                       "  --system-prompt <text>         System prompt (default: coding assistant\n"
                       "                                 prompt)\n"
                       "  --append-system-prompt <text>  Append text or file contents to the system\n"
                       "                                 prompt (can be used multiple times)\n"
                       "  --skill <path>                 Load a skill file or directory (can be used\n"
                       "                                 multiple times)\n"
                       "  --no-skills, -ns               Disable skills discovery and loading\n"
                       "  --prompt-template <path>       Load a prompt template file or directory (can\n"
                       "                                 be used multiple times)\n"
                       "  --no-prompt-templates, -np     Disable prompt template discovery and loading\n"
                       "  --theme <path>                 Load a theme file or directory (can be used\n"
                       "                                 multiple times)\n"
                       "  --no-themes                    Disable theme discovery and loading\n"
                       "  --no-context-files, -nc        Disable AGENTS.md and CLAUDE.md discovery and\n"
                       "                                 loading\n"
                       "  --list-models [search]         List available models (with optional fuzzy\n"
                       "                                 search)\n"
                       "  --help, -h                     Show this help\n"
                       "  --version, -v                  Show version number\n"
                       "\n"
                       "Sessions: without --session/--resume/--continue/--fork/--no-session, a new\n"
                       "session persists under the agent config directory's workspace-keyed sessions\n"
                       "root (~/.pi/agent/sessions/<workspace-key>/, root override:\n"
                       "PI_CODING_AGENT_DIR). Automatic-directory overrides (highest first):\n"
                       "--session-dir, PI_CODING_AGENT_SESSION_DIR, settings.json sessionDir;\n"
                       "relative values resolve against the workspace. Explicit paths may live\n"
                       "anywhere; --no-session runs in memory without a transcript.\n"
                       "Frontend: --mode text keeps automatic frontend selection (interactive TUI on\n"
                       "a terminal; --print or a non-TTY stream selects one-shot text).\n"
                       "Safety: prompts, file contents, and command outputs may be sent to the\n"
                       "configured provider. Sessions are local sensitive transcripts even after\n"
                       "secret-looking text is redacted.\n"};
}

[[nodiscard]] cch::support::Error parse_error(std::string message) {
    return cch::support::make_error(
        cch::support::ErrorCode::Validation,
        message,
        message + "\n\n" + help_text());
}

struct OptionToken {
    std::string_view name;
    std::string_view inline_value;
    bool has_inline_value{false};
};

[[nodiscard]] OptionToken split_option(std::string_view token) {
    const auto equals = token.find('=');
    if (equals == std::string_view::npos) {
        return OptionToken{
            .name = token.substr(2),
            .inline_value = {},
            .has_inline_value = false,
        };
    }
    return OptionToken{
        .name = token.substr(2, equals - 2),
        .inline_value = token.substr(equals + 1),
        .has_inline_value = true,
    };
}

[[nodiscard]] bool is_negative_number(std::string_view token) {
    if (token.size() < 2 || token.front() != '-') {
        return false;
    }
    if (std::isdigit(static_cast<unsigned char>(token[1])) != 0) {
        return true;
    }
    return token[1] == '.' && token.size() > 2 &&
        std::isdigit(static_cast<unsigned char>(token[2])) != 0;
}

/// Match the former option classifier for the retained argv surface. A lone `-`,
/// negative number, or token beginning with three dashes is a positional;
/// other short/long-looking tokens are options and are diagnosed if unknown.
[[nodiscard]] bool is_option_token(std::string_view token) {
    if (token.size() > 2 && token.starts_with("--") && token[2] != '-') {
        return static_cast<unsigned char>(token[2]) > 33;
    }
    if (token.size() > 1 && token.front() == '-' && token[1] != '-') {
        return !is_negative_number(token) &&
            static_cast<unsigned char>(token[1]) > 33;
    }
    return false;
}

[[nodiscard]] cch::support::Expected<std::string> consume_option_value(
    std::string_view option_name,
    const OptionToken& option,
    const std::vector<std::string>& tokens,
    std::size_t& index) {
    if (option.has_inline_value) {
        return std::string{option.inline_value};
    }
    if (index + 1 >= tokens.size()) {
        return std::unexpected(parse_error(
            std::format("--{}: 1 required TEXT missing", option_name)));
    }
    ++index;
    return tokens[index];
}

[[nodiscard]] cch::support::Expected<bool> parse_flag_value(
    std::string_view original_token,
    const OptionToken& option) {
    if (!option.has_inline_value || option.inline_value.empty()) {
        return true;
    }
    if (option.inline_value == "true" || option.inline_value == "1") {
        return true;
    }
    if (option.inline_value == "false" || option.inline_value == "0") {
        return false;
    }
    return std::unexpected(parse_error(std::format(
        "Could not convert: {} = {}",
        original_token.substr(0, original_token.find('=')),
        option.inline_value)));
}

[[nodiscard]] std::vector<std::string> split_model_patterns(
    std::string_view models_text) {
    std::vector<std::string> patterns;
    std::size_t start = 0;
    while (start <= models_text.size()) {
        const auto comma = models_text.find(',', start);
        const auto end = comma == std::string_view::npos ? models_text.size() : comma;
        std::string pattern{models_text.substr(start, end - start)};
        const auto not_space = [](unsigned char character) {
            return !std::isspace(character);
        };
        pattern.erase(pattern.begin(), std::find_if(pattern.begin(), pattern.end(), not_space));
        pattern.erase(
            std::find_if(pattern.rbegin(), pattern.rend(), not_space).base(),
            pattern.end());
        if (!pattern.empty()) {
            patterns.push_back(std::move(pattern));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return patterns;
}

} // namespace

std::string_view project_version() {
    return kProjectVersion;
}

cch::support::Expected<CliConfig> parse_args(int argc, char** argv) {
    CliConfig config;
    std::error_code cwd_ec;
    config.workspace = std::filesystem::current_path(cwd_ec);
    if (cwd_ec) {
        return std::unexpected(cch::support::make_error(
            cch::support::ErrorCode::Validation,
            "could not determine the current working directory",
            "the default workspace is unavailable: " + cwd_ec.message()));
    }

    std::vector<std::string> raw_args;
    raw_args.reserve(static_cast<std::size_t>(argc > 0 ? argc : 0));
    // argv[0] remains in the normalized vector for the conventional argv
    // shape; the manual parser starts with the user-supplied tokens at index 1.
    if (argc > 0) {
        raw_args.emplace_back(argv[0]);
    }
    for (int index = 1; index < argc; ++index) {
        raw_args.emplace_back(argv[index]);
    }
    auto normalized = normalize_argv(raw_args);

    std::string session_text;
    std::string session_dir_text;
    std::vector<std::string> prompt_parts;
    std::string provider_text;
    std::string model_text;
    std::string models_text;
    std::string api_key_text;
    std::string thinking_text;
    std::string session_id_text;
    std::string fork_text;
    std::string name_text;
    std::string system_prompt_text;
    std::string mode_text{"text"};
    bool session_seen = false;
    bool session_dir_seen = false;
    bool provider_seen = false;
    bool model_seen = false;
    bool models_seen = false;
    bool api_key_seen = false;
    bool thinking_seen = false;
    bool session_id_seen = false;
    bool fork_seen = false;
    bool name_seen = false;
    bool system_prompt_seen = false;
    bool mode_seen = false;
    bool approve_seen = false;
    bool approve_value = true;
    bool no_approve_seen = false;
    bool no_approve_value = true;
    std::vector<std::string> unknown_options;
    bool options_ended = false;

    for (std::size_t index = 1; index < normalized.tokens.size(); ++index) {
        const auto& token = normalized.tokens[index];
        if (options_ended) {
            prompt_parts.push_back(token);
            continue;
        }
        if (token == "--") {
            options_ended = true;
            continue;
        }
        if (!is_option_token(token)) {
            prompt_parts.push_back(token);
            continue;
        }
        if (!token.starts_with("--")) {
            unknown_options.push_back(token);
            continue;
        }

        const auto option = split_option(token);
        if (option.name == "help") {
            config.help = true;
            config.help_text = help_text();
            return config;
        }
        if (option.name == "version") {
            if (auto value = parse_flag_value(token, option); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                config.version = *value;
            }
            continue;
        }
        if (option.name == "print") {
            if (auto value = parse_flag_value(token, option); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                config.print = *value;
            }
            continue;
        }
        if (option.name == "approve") {
            if (auto value = parse_flag_value(token, option); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                approve_value = *value;
            }
            approve_seen = true;
            continue;
        }
        if (option.name == "no-approve") {
            if (auto value = parse_flag_value(token, option); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                no_approve_value = *value;
            }
            no_approve_seen = true;
            continue;
        }
        if (option.name == "no-skills") {
            if (auto value = parse_flag_value(token, option); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                config.session_facts.no_skills = *value;
            }
            continue;
        }
        if (option.name == "no-prompt-templates") {
            if (auto value = parse_flag_value(token, option); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                config.session_facts.no_prompt_templates = *value;
            }
            continue;
        }
        if (option.name == "no-context-files") {
            if (auto value = parse_flag_value(token, option); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                config.session_facts.no_context_files = *value;
            }
            continue;
        }
        if (option.name == "no-themes") {
            if (auto value = parse_flag_value(token, option); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                config.session_facts.no_themes = *value;
            }
            continue;
        }

        if (option.name == "prompt-template") {
            if (auto value = consume_option_value(option.name, option, normalized.tokens, index); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                config.session_facts.prompt_template_paths.push_back(std::move(*value));
            }
            continue;
        }
        if (option.name == "skill") {
            if (auto value = consume_option_value(option.name, option, normalized.tokens, index); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                config.session_facts.skill_paths.push_back(std::move(*value));
            }
            continue;
        }
        if (option.name == "theme") {
            if (auto value = consume_option_value(option.name, option, normalized.tokens, index); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                config.session_facts.theme_paths.push_back(std::move(*value));
            }
            continue;
        }
        if (option.name == "session") {
            if (auto value = consume_option_value(option.name, option, normalized.tokens, index); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                session_text = std::move(*value);
                session_seen = true;
            }
            continue;
        }
        if (option.name == "resume") {
            if (auto value = parse_flag_value(token, option); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                config.resume = *value;
            }
            continue;
        }
        if (option.name == "no-session") {
            if (auto value = parse_flag_value(token, option); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                config.no_session_flag = *value;
            }
            continue;
        }
        if (option.name == "session-dir") {
            if (auto value = consume_option_value(option.name, option, normalized.tokens, index); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                session_dir_text = std::move(*value);
                session_dir_seen = true;
            }
            continue;
        }
        if (option.name == "continue") {
            if (auto value = parse_flag_value(token, option); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                config.continue_session = *value;
            }
            continue;
        }
        if (option.name == "session-id") {
            if (auto value = consume_option_value(option.name, option, normalized.tokens, index); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                session_id_text = std::move(*value);
                session_id_seen = true;
            }
            continue;
        }
        if (option.name == "fork") {
            if (auto value = consume_option_value(option.name, option, normalized.tokens, index); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                fork_text = std::move(*value);
                fork_seen = true;
            }
            continue;
        }
        if (option.name == "name") {
            if (auto value = consume_option_value(option.name, option, normalized.tokens, index); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                name_text = std::move(*value);
                name_seen = true;
            }
            continue;
        }
        if (option.name == "provider") {
            if (auto value = consume_option_value(option.name, option, normalized.tokens, index); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                provider_text = std::move(*value);
                provider_seen = true;
            }
            continue;
        }
        if (option.name == "model") {
            if (auto value = consume_option_value(option.name, option, normalized.tokens, index); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                model_text = std::move(*value);
                model_seen = true;
            }
            continue;
        }
        if (option.name == "models") {
            if (auto value = consume_option_value(option.name, option, normalized.tokens, index); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                models_text = std::move(*value);
                models_seen = true;
            }
            continue;
        }
        if (option.name == "api-key") {
            if (auto value = consume_option_value(option.name, option, normalized.tokens, index); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                api_key_text = std::move(*value);
                api_key_seen = true;
            }
            continue;
        }
        if (option.name == "thinking") {
            if (auto value = consume_option_value(option.name, option, normalized.tokens, index); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                thinking_text = std::move(*value);
                thinking_seen = true;
            }
            continue;
        }
        if (option.name == "system-prompt") {
            if (auto value = consume_option_value(option.name, option, normalized.tokens, index); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                system_prompt_text = std::move(*value);
                system_prompt_seen = true;
            }
            continue;
        }
        if (option.name == "append-system-prompt") {
            if (auto value = consume_option_value(option.name, option, normalized.tokens, index); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                config.session_facts.append_system_prompt.push_back(std::move(*value));
            }
            continue;
        }
        if (option.name == "mode") {
            if (auto value = consume_option_value(option.name, option, normalized.tokens, index); !value) {
                return std::unexpected(std::move(value.error()));
            } else {
                mode_text = std::move(*value);
                mode_seen = true;
            }
            continue;
        }

        unknown_options.push_back(token);
    }

    if (!unknown_options.empty()) {
        std::string message{"unknown option: "};
        for (std::size_t index = 0; index < unknown_options.size(); ++index) {
            if (index > 0) {
                message += ' ';
            }
            message += unknown_options[index];
        }
        return std::unexpected(parse_error(std::move(message)));
    }

    if (approve_seen && no_approve_seen) {
        // Preserve the established normalized diagnostic for the mutually
        // exclusive project-trust flags.
        return std::unexpected(parse_error(
            "use either --session or --resume, not both"));
    }

    if (session_seen && !session_text.empty()) {
        config.session_value = std::move(session_text);
    }
    if (session_dir_seen) {
        config.session_dir = std::move(session_dir_text);
    }
    if (model_seen) {
        config.session_facts.model = std::move(model_text);
    }
    if (provider_seen) {
        config.session_facts.provider = std::move(provider_text);
    }
    if (models_seen) {
        config.session_facts.models = split_model_patterns(models_text);
    }
    if (api_key_seen) {
        config.session_facts.api_key = std::move(api_key_text);
    }
    if (thinking_seen) {
        config.thinking = std::move(thinking_text);
    }
    if (session_id_seen) {
        config.session_id = std::move(session_id_text);
    }
    if (fork_seen && !fork_text.empty()) {
        config.fork = std::move(fork_text);
    }
    if (name_seen) {
        config.name = std::move(name_text);
    }
    if (normalized.list_models) {
        config.list_models = std::move(*normalized.list_models);
    }
    if (system_prompt_seen) {
        config.session_facts.system_prompt = std::move(system_prompt_text);
    }
    if (approve_seen) {
        config.session_facts.project_trust_override = approve_value;
    } else if (no_approve_seen) {
        config.session_facts.project_trust_override = !no_approve_value;
    }

    if (mode_seen) {
        if (auto parsed_mode = validate_output_mode(mode_text); !parsed_mode) {
            return std::unexpected(std::move(parsed_mode.error()));
        }
    }

    // `--api-key` requires an explicit model (pi): it cannot name a provider
    // without a model, and never applies to a resume/default selection.
    if (api_key_seen && !model_seen && !provider_seen && !models_seen) {
        return std::unexpected(cli_error(
            "--api-key requires a model to be specified via --model, --provider/--model, or --models"));
    }

    std::vector<std::string> prompt_text_parts;
    prompt_text_parts.reserve(prompt_parts.size());
    for (auto& part : prompt_parts) {
        if (part.starts_with('@')) {
            config.file_arguments.push_back(part.substr(1));
        } else {
            prompt_text_parts.push_back(std::move(part));
        }
    }
    // pi keeps every positional as its own message: the first merges into the
    // initial prompt, the rest prompt sequentially in print mode.
    config.messages = std::move(prompt_text_parts);
    return config;
}

} // namespace cch::cli
