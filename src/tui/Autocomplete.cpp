#include <cch/tui/Autocomplete.hpp>

#include <cch/tui/Fuzzy.hpp>

#include "tui/InteractionUtils.hpp"
#include "support/UniqueFd.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cch::tui {
namespace {

// Behavioral baseline: pi 83114817 packages/tui/src/autocomplete.ts
// (CombinedAutocompleteProvider; the fd walk runs on a worker thread instead
// of pi's event-loop child process, with the same observable results,
// cancellation, and graceful-absence semantics).

constexpr std::size_t kFdMaxResults = 100;
constexpr std::size_t kFdTopResults = 20;

const std::string kPathDelimiters = " \t\"'=";

struct PathPrefix {
    std::string raw_prefix;
    bool is_at_prefix{false};
    bool is_quoted_prefix{false};
};

struct FdEntry {
    std::string path;
    bool is_directory{false};
};

[[nodiscard]] std::vector<std::string_view> split_view(std::string_view text, char separator) {
    std::vector<std::string_view> result;
    std::size_t begin = 0;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        if (index != text.size() && text[index] != separator) continue;
        result.push_back(text.substr(begin, index - begin));
        begin = index + 1;
    }
    return result;
}

[[nodiscard]] std::string to_display_path(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char ch : value) result.push_back(ch == '\\' ? '/' : ch);
    return result;
}

[[nodiscard]] std::string escape_regex(std::string_view value) {
    constexpr std::string_view special = ".*+?^${}()|[]\\";
    std::string result;
    for (const char ch : value) {
        if (special.find(ch) != std::string_view::npos) result.push_back('\\');
        result.push_back(ch);
    }
    return result;
}

[[nodiscard]] std::string build_fd_path_query(std::string_view query) {
    const auto normalized = to_display_path(query);
    if (normalized.find('/') == std::string::npos) return normalized;

    const bool has_trailing_separator = normalized.ends_with('/');
    const auto first = normalized.find_first_not_of('/');
    const auto last = normalized.find_last_not_of('/');
    if (first == std::string::npos) return normalized;

    const auto trimmed = normalized.substr(first, last - first + 1);
    constexpr std::string_view separator_pattern = R"([\\/])";
    std::string pattern;
    bool first_segment = true;
    std::size_t begin = 0;
    for (std::size_t index = 0; index <= trimmed.size(); ++index) {
        if (index != trimmed.size() && trimmed[index] != '/') continue;
        if (index > begin) {
            if (!first_segment) pattern += separator_pattern;
            pattern += escape_regex(trimmed.substr(begin, index - begin));
            first_segment = false;
        }
        begin = index + 1;
    }
    if (has_trailing_separator) pattern += separator_pattern;
    return pattern;
}

[[nodiscard]] std::optional<std::size_t> find_last_delimiter(std::string_view text) {
    for (std::size_t index = text.size(); index-- > 0;) {
        if (kPathDelimiters.find(text[index]) != std::string::npos) return index;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::size_t> find_unclosed_quote_start(std::string_view text) {
    bool in_quotes = false;
    std::optional<std::size_t> quote_start;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '"') continue;
        in_quotes = !in_quotes;
        quote_start = index;
    }
    return in_quotes ? quote_start : std::nullopt;
}

[[nodiscard]] bool is_token_start(std::string_view text, std::size_t index) {
    return index == 0 || kPathDelimiters.find(text[index - 1]) != std::string::npos;
}

[[nodiscard]] std::optional<std::string_view> extract_quoted_prefix(std::string_view text) {
    const auto quote_start = find_unclosed_quote_start(text);
    if (!quote_start) return std::nullopt;

    if (*quote_start > 0 && text[*quote_start - 1] == '@') {
        if (!is_token_start(text, *quote_start - 1)) return std::nullopt;
        return text.substr(*quote_start - 1);
    }
    if (!is_token_start(text, *quote_start)) return std::nullopt;
    return text.substr(*quote_start);
}

[[nodiscard]] PathPrefix parse_path_prefix(std::string_view prefix) {
    if (prefix.starts_with("@\"")) {
        return {.raw_prefix = std::string{prefix.substr(2)}, .is_at_prefix = true, .is_quoted_prefix = true};
    }
    if (prefix.starts_with('"')) {
        return {.raw_prefix = std::string{prefix.substr(1)}, .is_at_prefix = false, .is_quoted_prefix = true};
    }
    if (prefix.starts_with('@')) {
        return {.raw_prefix = std::string{prefix.substr(1)}, .is_at_prefix = true, .is_quoted_prefix = false};
    }
    return {.raw_prefix = std::string{prefix}, .is_at_prefix = false, .is_quoted_prefix = false};
}

[[nodiscard]] std::string build_completion_value(
    std::string_view path,
    bool is_at_prefix,
    bool is_quoted_prefix) {
    const bool needs_quotes = is_quoted_prefix || path.find(' ') != std::string_view::npos;
    const std::string prefix = is_at_prefix ? "@" : "";
    if (!needs_quotes) return prefix + std::string{path};
    return prefix + "\"" + std::string{path} + "\"";
}

[[nodiscard]] std::string home_directory() {
    if (const char* home = std::getenv("HOME")) return home;
    return {};
}

[[nodiscard]] std::string expand_home_path(std::string_view path) {
    if (path.starts_with("~/")) {
        auto expanded = home_directory() + "/" + std::string{path.substr(2)};
        if (path.ends_with('/') && !expanded.ends_with('/')) expanded.push_back('/');
        return expanded;
    }
    if (path == "~") return home_directory();
    return std::string{path};
}

/// Concatenate display-path segments with '/' without normalization (pi's
/// path.join on the display path).
[[nodiscard]] std::string join_path(std::string_view left, std::string_view right) {
    if (left.empty()) return std::string{right};
    if (right.empty()) return std::string{left};
    if (left.ends_with('/')) return std::string{left} + std::string{right};
    return std::string{left} + "/" + std::string{right};
}

[[nodiscard]] std::string_view dirname_of(std::string_view path) {
    const auto slash = path.rfind('/');
    if (slash == std::string_view::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

[[nodiscard]] std::string_view basename_of(std::string_view path) {
    const auto slash = path.rfind('/');
    if (slash == std::string_view::npos) return path;
    return path.substr(slash + 1);
}

[[nodiscard]] bool starts_with_case_insensitive(std::string_view value, std::string_view prefix) {
    if (prefix.size() > value.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        const auto left = static_cast<unsigned char>(value[index]);
        const auto right = static_cast<unsigned char>(prefix[index]);
        if (std::tolower(left) != std::tolower(right)) return false;
    }
    return true;
}

struct ScopedQuery {
    std::filesystem::path base_dir;
    std::string query;
    std::string display_base;
};

[[nodiscard]] std::optional<ScopedQuery> resolve_scoped_fuzzy_query(
    const std::filesystem::path& base_path,
    std::string_view raw_query) {
    const auto normalized = to_display_path(raw_query);
    const auto slash_index = normalized.rfind('/');
    if (slash_index == std::string::npos) return std::nullopt;

    const auto display_base = normalized.substr(0, slash_index + 1);
    const auto query = normalized.substr(slash_index + 1);

    std::filesystem::path base_dir;
    if (display_base.starts_with("~/")) {
        base_dir = expand_home_path(display_base);
    } else if (display_base.starts_with('/')) {
        base_dir = display_base;
    } else {
        base_dir = join_path(base_path.string(), display_base);
    }

    std::error_code error;
    if (!std::filesystem::is_directory(base_dir, error) || error) return std::nullopt;
    return ScopedQuery{.base_dir = std::move(base_dir), .query = query, .display_base = display_base};
}

[[nodiscard]] std::string scoped_path_for_display(std::string_view display_base, std::string_view relative_path) {
    const auto normalized = to_display_path(relative_path);
    if (display_base == "/") return "/" + normalized;
    return to_display_path(display_base) + normalized;
}

/// Walk the directory tree with an `fd` child process (pi's
/// `walkDirectoryWithFd`). Runs on the caller's thread; the child is killed
/// when `stop_token` is requested.
[[nodiscard]] std::vector<FdEntry> walk_directory_with_fd(
    const std::filesystem::path& base_dir,
    const std::filesystem::path& fd_path,
    std::string_view query,
    std::size_t max_results,
    std::stop_token stop_token) {
    if (stop_token.stop_requested()) return {};

    std::vector<std::string> args{
        "--base-directory",
        base_dir.string(),
        "--max-results",
        std::to_string(max_results),
        "--type",
        "f",
        "--type",
        "d",
        "--follow",
        "--hidden",
        "--exclude",
        ".git",
        "--exclude",
        ".git/*",
        "--exclude",
        ".git/**",
    };
    if (to_display_path(query).find('/') != std::string::npos) {
        args.push_back("--full-path");
    }
    if (!query.empty()) {
        args.push_back(build_fd_path_query(query));
    }

    int pipes[2];
    if (pipe(pipes) != 0) return {};
    cch::support::UniqueFd read_end(pipes[0]);
    cch::support::UniqueFd write_end(pipes[1]);
    const auto pid = fork();
    if (pid < 0) return {};
    if (pid == 0) {
        // Child: run fd with stdout piped to the parent.
        dup2(write_end.get(), STDOUT_FILENO);
        read_end.reset();
        write_end.reset();
        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char*>(fd_path.c_str()));
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    write_end.reset();

    std::optional<std::stop_callback<std::function<void()>>> kill_on_stop;
    if (stop_token.stop_possible()) {
        kill_on_stop.emplace(stop_token, [pid] {
            if (pid > 0) ::kill(pid, SIGKILL);
        });
    }

    std::string stdout_buffer;
    char chunk[4096];
    for (;;) {
        const auto count = read(read_end.get(), chunk, sizeof(chunk));
        if (count < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (count == 0) break;
        stdout_buffer.append(chunk, static_cast<std::size_t>(count));
    }
    read_end.reset();

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }

    if (stop_token.stop_requested()) return {};
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || stdout_buffer.empty()) return {};

    std::vector<FdEntry> results;
    for (const auto line : split_view(stdout_buffer, '\n')) {
        if (line.empty()) continue;
        const auto display_line = to_display_path(line);
        const bool has_trailing_separator = display_line.ends_with('/');
        const std::string_view normalized_path =
            has_trailing_separator ? std::string_view{display_line}.substr(0, display_line.size() - 1)
                                   : std::string_view{display_line};
        if (normalized_path == ".git" || normalized_path.starts_with(".git/") ||
            normalized_path.find("/.git/") != std::string_view::npos) {
            continue;
        }
        results.push_back({.path = display_line, .is_directory = has_trailing_separator});
    }
    return results;
}

/// Score an entry against the query (pi `scoreEntry`); higher is better.
[[nodiscard]] double score_entry(std::string_view file_path, std::string_view query, bool is_directory) {
    const auto file_name = basename_of(file_path);
    std::string lower_file_name;
    lower_file_name.reserve(file_name.size());
    for (const char ch : file_name) {
        lower_file_name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    std::string lower_query;
    lower_query.reserve(query.size());
    for (const char ch : query) {
        lower_query.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    std::string lower_path;
    lower_path.reserve(file_path.size());
    for (const char ch : file_path) {
        lower_path.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    double score = 0;
    if (lower_file_name == lower_query) score = 100;
    else if (lower_file_name.starts_with(lower_query)) score = 80;
    else if (lower_file_name.find(lower_query) != std::string::npos) score = 50;
    else if (lower_path.find(lower_query) != std::string::npos) score = 30;

    if (is_directory && score > 0) score += 10;
    return score;
}

[[nodiscard]] std::vector<AutocompleteItem> get_file_suggestions(
    const std::filesystem::path& base_path,
    std::string_view prefix) {
    const auto parsed = parse_path_prefix(prefix);
        const auto& raw_prefix = parsed.raw_prefix;
        auto expanded_prefix = raw_prefix;
        if (expanded_prefix.starts_with('~')) expanded_prefix = expand_home_path(expanded_prefix);

        const bool is_root_prefix = raw_prefix.empty() || raw_prefix == "./" || raw_prefix == "../" ||
            raw_prefix == "~" || raw_prefix == "~/" || raw_prefix == "/" ||
            (parsed.is_at_prefix && raw_prefix.empty());

        std::filesystem::path search_dir;
        std::string search_prefix;
        if (is_root_prefix) {
            if (raw_prefix.starts_with('~') || expanded_prefix.starts_with('/')) {
                search_dir = expanded_prefix;
            } else {
                search_dir = join_path(base_path.string(), expanded_prefix);
            }
            search_prefix = "";
        } else if (raw_prefix.ends_with('/')) {
            if (raw_prefix.starts_with('~') || expanded_prefix.starts_with('/')) {
                search_dir = expanded_prefix;
            } else {
                search_dir = join_path(base_path.string(), expanded_prefix);
            }
            search_prefix = "";
        } else {
            const auto dir = dirname_of(expanded_prefix);
            const auto file = basename_of(expanded_prefix);
            if (raw_prefix.starts_with('~') || expanded_prefix.starts_with('/')) {
                search_dir = std::filesystem::path{dir};
            } else {
                search_dir = join_path(base_path.string(), dir);
            }
            search_prefix = std::string{file};
        }

        std::vector<AutocompleteItem> suggestions;
        std::error_code error;
        auto iterator = std::filesystem::directory_iterator(
            search_dir, std::filesystem::directory_options::skip_permission_denied, error);
        if (error) return {};
        for (;;) {
            if (iterator == std::filesystem::directory_iterator{}) break;
            const auto& entry = *iterator;
            const auto& name = entry.path().filename().string();
            std::error_code entry_error;
            if (!starts_with_case_insensitive(name, search_prefix)) {
                iterator.increment(entry_error);
                if (entry_error) return {};
                continue;
            }

            bool is_directory = entry.is_directory(entry_error);
            if (entry_error) {
                // Permission error - treat as file.
                is_directory = false;
            }
            if (!is_directory && entry.is_symlink(entry_error)) {
                is_directory = std::filesystem::is_directory(entry.path(), entry_error);
                if (entry_error) {
                    // Broken symlink or permission error - treat as file.
                    is_directory = false;
                }
            }

                std::string relative_path;
                const auto& display_prefix = raw_prefix;
                if (display_prefix.ends_with('/')) {
                    relative_path = display_prefix + name;
                } else if (display_prefix.find('/') != std::string::npos ||
                           display_prefix.find('\\') != std::string::npos) {
                    if (display_prefix.starts_with("~/")) {
                        const auto home_relative_dir = std::string_view{display_prefix}.substr(2);
                        const auto dir = dirname_of(home_relative_dir);
                        relative_path = dir == "." ? "~/" + name : "~/" + join_path(dir, name);
                    } else if (display_prefix.starts_with('/')) {
                        const auto dir = dirname_of(display_prefix);
                        relative_path = dir == "/" ? "/" + name : join_path(dir, name);
                    } else {
                        relative_path = join_path(dirname_of(display_prefix), name);
                        if (display_prefix.starts_with("./") && !relative_path.starts_with("./")) {
                            relative_path = "./" + relative_path;
                        }
                    }
                } else {
                    relative_path = display_prefix.starts_with('~') ? "~/" + name : name;
                }

                relative_path = to_display_path(relative_path);
                const auto path_value = is_directory ? relative_path + "/" : relative_path;
                const auto value = build_completion_value(
                    path_value,
                    parsed.is_at_prefix,
                    parsed.is_quoted_prefix);

            suggestions.push_back({
                .value = value,
                .label = name + (is_directory ? "/" : ""),
                .description = {},
            });
            iterator.increment(entry_error);
            if (entry_error) return {};
        }

        std::stable_sort(suggestions.begin(), suggestions.end(), [](const AutocompleteItem& left, const AutocompleteItem& right) {
            const bool left_dir = left.value.ends_with('/');
            const bool right_dir = right.value.ends_with('/');
            if (left_dir != right_dir) return left_dir;
            return left.label < right.label;
        });
        return suggestions;
}

[[nodiscard]] std::vector<AutocompleteItem> get_fuzzy_file_suggestions(
    const std::filesystem::path& base_path,
    const std::filesystem::path& fd_path,
    std::string_view query,
    bool is_quoted_prefix,
    std::stop_token stop_token) {
    if (stop_token.stop_requested()) return {};

#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        const auto scoped_query = resolve_scoped_fuzzy_query(base_path, query);
        const auto fd_base_dir = scoped_query ? scoped_query->base_dir : base_path;
        const auto fd_query = scoped_query ? scoped_query->query : std::string{query};
        const auto entries = walk_directory_with_fd(fd_base_dir, fd_path, fd_query, kFdMaxResults, stop_token);
        if (stop_token.stop_requested()) return {};

        struct ScoredEntry {
            std::string path;
            bool is_directory{false};
            double score{0};
        };
        std::vector<ScoredEntry> scored;
        for (const auto& entry : entries) {
            const auto score = fd_query.empty() ? 1.0 : score_entry(entry.path, fd_query, entry.is_directory);
            if (score <= 0) continue;
            scored.push_back({.path = entry.path, .is_directory = entry.is_directory, .score = score});
        }

        std::stable_sort(scored.begin(), scored.end(), [](const ScoredEntry& left, const ScoredEntry& right) {
            return left.score > right.score;
        });
        if (scored.size() > kFdTopResults) scored.resize(kFdTopResults);

        std::vector<AutocompleteItem> suggestions;
        suggestions.reserve(scored.size());
        for (const auto& entry : scored) {
            const auto path_without_slash = entry.is_directory && entry.path.ends_with('/')
                ? std::string_view{entry.path}.substr(0, entry.path.size() - 1)
                : std::string_view{entry.path};
            const auto display_path = scoped_query
                ? scoped_path_for_display(scoped_query->display_base, path_without_slash)
                : std::string{path_without_slash};
            const auto entry_name = basename_of(path_without_slash);
            const auto completion_path = entry.is_directory ? display_path + "/" : display_path;
            const auto value = build_completion_value(completion_path, true, is_quoted_prefix);

            suggestions.push_back({
                .value = value,
                .label = std::string{entry_name} + (entry.is_directory ? "/" : ""),
                .description = display_path,
            });
        }
        return suggestions;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (...) {
        return {};
    }
#endif
}

/// Extract the `@` prefix for fuzzy file suggestions (pi `extractAtPrefix`).
[[nodiscard]] std::optional<std::string_view> extract_at_prefix(std::string_view text) {
    const auto quoted_prefix = extract_quoted_prefix(text);
    if (quoted_prefix && quoted_prefix->starts_with("@\"")) {
        return quoted_prefix;
    }

    const auto last_delimiter_index = find_last_delimiter(text);
    const auto token_start = last_delimiter_index ? *last_delimiter_index + 1 : 0U;
    if (token_start < text.size() && text[token_start] == '@') {
        return text.substr(token_start);
    }
    return std::nullopt;
}

/// Extract a path-like prefix from the text before cursor (pi
/// `extractPathPrefix`).
[[nodiscard]] std::optional<std::string_view> extract_path_prefix(
    std::string_view text,
    bool force_extract) {
    const auto quoted_prefix = extract_quoted_prefix(text);
    if (quoted_prefix) {
        return quoted_prefix;
    }

    const auto last_delimiter_index = find_last_delimiter(text);
    const auto path_prefix = last_delimiter_index ? text.substr(*last_delimiter_index + 1) : text;

    if (force_extract) {
        return path_prefix;
    }

    if (path_prefix.find('/') != std::string_view::npos || path_prefix.starts_with('.') ||
        path_prefix.starts_with("~/")) {
        return path_prefix;
    }
    if (path_prefix.empty() && !text.empty() && text.back() == ' ') {
        return path_prefix;
    }
    return std::nullopt;
}

struct CommandEntry {
    std::string name;
    std::string label;
    std::string description;
};

} // namespace

struct CombinedAutocompleteProvider::Impl {
    std::vector<std::variant<SlashCommand, AutocompleteItem>> commands;
    std::filesystem::path base_path;
    std::optional<std::filesystem::path> fd_path;
};

CombinedAutocompleteProvider::CombinedAutocompleteProvider(
    std::vector<std::variant<SlashCommand, AutocompleteItem>> commands,
    std::filesystem::path base_path,
    std::optional<std::filesystem::path> fd_path)
    : impl_(std::make_unique<Impl>(Impl{
          .commands = std::move(commands),
          .base_path = std::move(base_path),
          .fd_path = std::move(fd_path),
      })) {}

CombinedAutocompleteProvider::~CombinedAutocompleteProvider() = default;
CombinedAutocompleteProvider::CombinedAutocompleteProvider(CombinedAutocompleteProvider&&) noexcept = default;
CombinedAutocompleteProvider& CombinedAutocompleteProvider::operator=(CombinedAutocompleteProvider&&) noexcept =
    default;

void CombinedAutocompleteProvider::get_suggestions(
    const AutocompleteRequest& request,
    AutocompleteResultSink sink) {
    const auto current_line =
        request.cursor_line < request.lines.size() ? std::string_view{request.lines[request.cursor_line]}
                                                   : std::string_view{};
    const auto text_before_cursor = current_line.substr(0, std::min(request.cursor_column, current_line.size()));

    // `@` attachment prefix: fuzzy fd-backed completion (pi's at-prefix branch).
    if (const auto at_prefix = extract_at_prefix(text_before_cursor)) {
        const auto parsed = parse_path_prefix(*at_prefix);
        if (!impl_->fd_path || request.stop_token.stop_requested()) {
            (void)sink(std::nullopt);
            return;
        }
        const std::string at_prefix_copy{*at_prefix};
        const std::string raw_prefix = parsed.raw_prefix;
        const bool is_quoted_prefix = parsed.is_quoted_prefix;
        const auto base_path = impl_->base_path;
        const auto fd_path = *impl_->fd_path;
        const auto stop_token = request.stop_token;
        std::thread([base_path, fd_path, raw_prefix, is_quoted_prefix, at_prefix_copy, stop_token,
                     sink = std::move(sink)]() mutable {
            const auto suggestions =
                get_fuzzy_file_suggestions(base_path, fd_path, raw_prefix, is_quoted_prefix, stop_token);
            if (suggestions.empty()) {
                (void)sink(std::nullopt);
                return;
            }
            (void)sink(AutocompleteSuggestions{
                .items = std::move(suggestions),
                .prefix = at_prefix_copy,
            });
        }).detach();
        return;
    }

    // Slash commands (not on a forced request; pi's `!options.force` gate).
    if (!request.force && text_before_cursor.starts_with('/')) {
        const auto space_index = text_before_cursor.find(' ');
        if (space_index == std::string_view::npos) {
            const auto prefix = text_before_cursor.substr(1);

            std::vector<CommandEntry> command_items;
            command_items.reserve(impl_->commands.size());
            for (const auto& command : impl_->commands) {
                std::visit(
                    [&command_items](const auto& command_value) {
                        using T = std::decay_t<decltype(command_value)>;
                        if constexpr (std::is_same_v<T, SlashCommand>) {
                            const auto hint = command_value.argument_hint;
                            const auto desc = command_value.description;
                            const auto full_desc =
                                !hint.empty() ? (desc.empty() ? hint : hint + " \u2014 " + desc) : desc;
                            command_items.push_back({
                                .name = command_value.name,
                                .label = command_value.name,
                                .description = full_desc,
                            });
                        } else {
                            command_items.push_back({
                                .name = command_value.value,
                                .label = command_value.label,
                                .description = command_value.description,
                            });
                        }
                    },
                    command);
            }

            const auto filtered = fuzzy_filter(
                std::move(command_items),
                prefix,
                [](const CommandEntry& entry) -> const std::string& { return entry.name; });
            if (filtered.empty()) {
                (void)sink(std::nullopt);
                return;
            }
            std::vector<AutocompleteItem> items;
            items.reserve(filtered.size());
            for (const auto& entry : filtered) {
                items.push_back({
                    .value = entry.name,
                    .label = entry.label,
                    .description = entry.description,
                });
            }
            (void)sink(AutocompleteSuggestions{
                .items = std::move(items),
                .prefix = std::string{text_before_cursor},
            });
            return;
        }

        // Command argument completion after `/name `.
        const auto command_name = text_before_cursor.substr(1, space_index - 1);
        const auto argument_text = text_before_cursor.substr(space_index + 1);
        for (auto& command : impl_->commands) {
            auto* slash_command = std::get_if<SlashCommand>(&command);
            if (slash_command == nullptr || slash_command->name != command_name) continue;
            if (!slash_command->get_argument_completions) {
                (void)sink(std::nullopt);
                return;
            }
            const auto argument_suggestions = slash_command->get_argument_completions(argument_text);
            if (!argument_suggestions || argument_suggestions->empty()) {
                (void)sink(std::nullopt);
                return;
            }
            (void)sink(AutocompleteSuggestions{
                .items = *std::move(argument_suggestions),
                .prefix = std::string{argument_text},
            });
            return;
        }
        (void)sink(std::nullopt);
        return;
    }

    // Path-like prefix (natural or forced Tab; pi's extractPathPrefix).
    const auto path_match = extract_path_prefix(text_before_cursor, request.force);
    if (!path_match) {
        (void)sink(std::nullopt);
        return;
    }
    const auto suggestions = get_file_suggestions(impl_->base_path, *path_match);
    if (suggestions.empty()) {
        (void)sink(std::nullopt);
        return;
    }
    (void)sink(AutocompleteSuggestions{
        .items = suggestions,
        .prefix = std::string{*path_match},
    });
}

AutocompleteApplyResult CombinedAutocompleteProvider::apply_completion(
    const std::vector<std::string>& lines,
    std::size_t cursor_line,
    std::size_t cursor_column,
    const AutocompleteItem& item,
    std::string_view prefix) {
    if (cursor_line >= lines.size()) {
        return {.lines = lines, .cursor_line = cursor_line, .cursor_column = cursor_column};
    }
    const auto& current_line = lines[cursor_line];
    const auto cursor_col = std::min(cursor_column, current_line.size());
    const auto before_prefix = current_line.substr(0, cursor_col - std::min(prefix.size(), cursor_col));
    const auto after_cursor = current_line.substr(cursor_col);
    const bool is_quoted_prefix = prefix.starts_with('"') || prefix.starts_with("@\"");
    const bool has_leading_quote_after_cursor = after_cursor.starts_with('"');
    const bool has_trailing_quote_in_item = item.value.ends_with('"');
    const auto adjusted_after_cursor =
        is_quoted_prefix && has_trailing_quote_in_item && has_leading_quote_after_cursor
        ? after_cursor.substr(1)
        : after_cursor;

    auto new_lines = lines;
    auto& new_line = new_lines[cursor_line];

    // Slash command name completion: prefix starts with "/" but is NOT a file
    // path (commands sit at the start of the line with no path separators).
    const bool is_slash_command = prefix.starts_with('/') && cch::tui::detail::trim_start_ascii(before_prefix).empty() &&
        prefix.substr(1).find('/') == std::string_view::npos;
    if (is_slash_command) {
        new_line = before_prefix + "/" + item.value + " " + adjusted_after_cursor;
        return {
            .lines = std::move(new_lines),
            .cursor_line = cursor_line,
            .cursor_column = before_prefix.size() + item.value.size() + 2,  // "/" and the trailing space
        };
    }

    // File attachment completion (`@` prefix): no trailing space after
    // directories so the user can keep autocompleting.
    if (prefix.starts_with('@')) {
        const bool is_directory = item.label.ends_with('/');
        const std::string_view suffix = is_directory ? "" : " ";
        new_line = before_prefix + item.value + std::string{suffix} + adjusted_after_cursor;
        const bool has_trailing_quote = item.value.ends_with('"');
        const auto cursor_offset = is_directory && has_trailing_quote ? item.value.size() - 1 : item.value.size();
        return {
            .lines = std::move(new_lines),
            .cursor_line = cursor_line,
            .cursor_column = before_prefix.size() + cursor_offset + suffix.size(),
        };
    }

    // Slash command argument context (`/command ` before the cursor).
    const auto text_before_cursor = current_line.substr(0, cursor_col);
    if (text_before_cursor.find('/') != std::string::npos && text_before_cursor.find(' ') != std::string::npos) {
        new_line = before_prefix + item.value + adjusted_after_cursor;
        const bool is_directory = item.label.ends_with('/');
        const bool has_trailing_quote = item.value.ends_with('"');
        const auto cursor_offset = is_directory && has_trailing_quote ? item.value.size() - 1 : item.value.size();
        return {
            .lines = std::move(new_lines),
            .cursor_line = cursor_line,
            .cursor_column = before_prefix.size() + cursor_offset,
        };
    }

    // Plain file path completion.
    new_line = before_prefix + item.value + adjusted_after_cursor;
    const bool is_directory = item.label.ends_with('/');
    const bool has_trailing_quote = item.value.ends_with('"');
    const auto cursor_offset = is_directory && has_trailing_quote ? item.value.size() - 1 : item.value.size();
    return {
        .lines = std::move(new_lines),
        .cursor_line = cursor_line,
        .cursor_column = before_prefix.size() + cursor_offset,
    };
}

bool CombinedAutocompleteProvider::should_trigger_file_completion(
    const std::vector<std::string>& lines,
    std::size_t cursor_line,
    std::size_t cursor_column) const {
    const auto current_line =
        cursor_line < lines.size() ? std::string_view{lines[cursor_line]} : std::string_view{};
    const auto text_before_cursor = current_line.substr(0, std::min(cursor_column, current_line.size()));
    // pi trims both ends (JS trim()): "/cmd " with a trailing space is still a
    // bare slash command and must not trigger file completion.
    const auto first = std::find_if_not(text_before_cursor.begin(), text_before_cursor.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(text_before_cursor.rbegin(), text_before_cursor.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (first >= last) return true;
    const auto trimmed = text_before_cursor.substr(
        static_cast<std::size_t>(first - text_before_cursor.begin()),
        static_cast<std::size_t>(last - first));
    if (trimmed.starts_with('/') && trimmed.find(' ') == std::string_view::npos) {
        return false;
    }
    return true;
}

std::vector<std::string> CombinedAutocompleteProvider::trigger_characters() const {
    return {};  // pi's CombinedAutocompleteProvider sets none; the editor defaults apply
}

} // namespace cch::tui
