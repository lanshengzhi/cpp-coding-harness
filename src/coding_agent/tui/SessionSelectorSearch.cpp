#include "SessionSelectorSearch.hpp"

#include <cch/tui/Fuzzy.hpp>

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <string_view>
#include <utility>

namespace cch::coding_agent::tui {
namespace {

[[nodiscard]] std::string normalize_whitespace_lower(std::string text) {
    std::string result;
    result.reserve(text.size());
    bool pending_space = false;
    for (const unsigned char character : text) {
        if (std::isspace(character) != 0) {
            pending_space = true;
            continue;
        }
        if (pending_space) {
            result.push_back(' ');
            pending_space = false;
        }
        result.push_back(static_cast<char>(std::tolower(character)));
    }
    const auto not_space = [](unsigned char character) {
        return std::isspace(character) == 0;
    };
    result.erase(result.begin(), std::find_if(result.begin(), result.end(), not_space));
    result.erase(std::find_if(result.rbegin(), result.rend(), not_space).base(), result.end());
    return result;
}

[[nodiscard]] std::string session_search_text(
    const session_discovery::SessionInfo& session) {
    std::string text = session.id;
    text.push_back(' ');
    if (session.name) {
        text += *session.name;
        text.push_back(' ');
    }
    text += session.all_messages_text;
    text.push_back(' ');
    text += session.cwd;
    return text;
}

/// A structural validity scan for ECMAScript regular-expression patterns.
/// The staged build additionally compiles the pattern with `std::regex`, which
/// reports malformed input through `std::regex_error`; with exceptions disabled
/// libstdc++ cannot report a malformed pattern (it aborts), so this scan rejects
/// the structural errors users commonly type — an unterminated character class,
/// an unmatched or unopened group, or a dangling escape — before any
/// `std::regex` is constructed from the untrusted pattern. A pattern that passes
/// this scan but is still rejected by the compiler remains a documented residual
/// risk in the no-exception build.
[[nodiscard]] bool structurally_valid_regex(std::string_view pattern) {
    std::size_t group_depth = 0;
    bool in_character_class = false;
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        const char character = pattern[index];
        if (character == '\\') {
            if (index + 1 >= pattern.size()) return false;
            ++index;  // the escaped character is literal
            continue;
        }
        if (in_character_class) {
            if (character == ']') in_character_class = false;
            continue;
        }
        switch (character) {
            case '[':
                in_character_class = true;
                break;
            case '(':
                ++group_depth;
                break;
            case ')':
                if (group_depth == 0) return false;
                --group_depth;
                break;
            default:
                break;
        }
    }
    return !in_character_class && group_depth == 0;
}
} // namespace

/// Trim ASCII whitespace and return the trimmed view.
[[nodiscard]] std::string_view trim_view(std::string_view text) {
    const auto begin = std::find_if_not(text.begin(), text.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
    const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char character) {
        return std::isspace(character) != 0;
    }).base();
    if (begin >= end) {
        return {};
    }
    return text.substr(
        static_cast<std::size_t>(begin - text.begin()),
        static_cast<std::size_t>(end - begin));
}


ParsedSessionQuery parse_session_search_query(std::string_view query) {
    ParsedSessionQuery parsed;
    const auto body = trim_view(query);
    if (body.empty()) {
        return parsed;
    }

    // Regex mode: re:<pattern>
    if (body.starts_with("re:")) {
        const auto pattern_trimmed = trim_view(body.substr(3));
        if (pattern_trimmed.empty()) {
            parsed.regex_mode = true;
            parsed.regex_error = "Empty regex";
            return parsed;
        }
        if (!structurally_valid_regex(pattern_trimmed)) {
            parsed.regex_mode = true;
            parsed.regex_error = "Invalid regular expression";
            return parsed;
        }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            (void)std::regex{
                std::string{pattern_trimmed},
                std::regex::ECMAScript | std::regex::icase};
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (const std::regex_error& error) {
            parsed.regex_mode = true;
            parsed.regex_error = error.what();
            return parsed;
        }
#endif
        parsed.regex_mode = true;
        parsed.regex_pattern = std::string{pattern_trimmed};
        return parsed;
    }

    // Token mode with quote support: foo "node cve" bar
    std::string buffer;
    bool in_quote = false;
    bool had_unclosed_quote = false;
    const auto flush = [&](bool phrase) {
        std::string value = std::string{trim_view(buffer)};
        buffer.clear();
        if (value.empty()) {
            return;
        }
        parsed.tokens.push_back(ParsedSessionQuery::Token{
            .phrase = phrase,
            .value = std::move(value),
        });
    };

    for (const char character : body) {
        if (character == '"') {
            if (in_quote) {
                flush(true);
                in_quote = false;
            } else {
                flush(false);
                in_quote = true;
            }
            continue;
        }
        if (!in_quote && std::isspace(static_cast<unsigned char>(character)) != 0) {
            flush(false);
            continue;
        }
        buffer.push_back(character);
    }
    if (in_quote) {
        had_unclosed_quote = true;
    }

    // Unbalanced quotes fall back to plain whitespace tokenization.
    if (had_unclosed_quote) {
        ParsedSessionQuery fallback;
        fallback.unclosed_quote = true;
        std::string token;
        for (const char character : body) {
            if (std::isspace(static_cast<unsigned char>(character)) != 0) {
                if (!token.empty()) {
                    fallback.tokens.push_back(ParsedSessionQuery::Token{
                        .phrase = false,
                        .value = std::move(token),
                    });
                    token.clear();
                }
                continue;
            }
            token.push_back(character);
        }
        if (!token.empty()) {
            fallback.tokens.push_back(ParsedSessionQuery::Token{
                .phrase = false,
                .value = std::move(token),
            });
        }
        return fallback;
    }

    flush(in_quote);
    return parsed;
}

bool has_session_name(const session_discovery::SessionInfo& session) {
    if (!session.name) {
        return false;
    }
    const auto not_space = [](unsigned char character) {
        return std::isspace(character) == 0;
    };
    return std::find_if(
               session.name->begin(), session.name->end(), not_space) !=
        session.name->end();
}

SessionMatchResult match_session(
    const session_discovery::SessionInfo& session,
    const ParsedSessionQuery& query) {
    const auto text = session_search_text(session);

    if (query.regex_mode) {
        if (query.regex_error || !query.regex_pattern) {
            return {.matches = false, .score = 0.0};
        }
        const std::regex expression{
            *query.regex_pattern,
            std::regex::ECMAScript | std::regex::icase};
        std::smatch match;
        if (!std::regex_search(text, match, expression)) {
            return {.matches = false, .score = 0.0};
        }
        return {
            .matches = true,
            .score = static_cast<double>(match.position()) * 0.1,
        };
    }

    if (query.tokens.empty()) {
        return {.matches = true, .score = 0.0};
    }

    double total_score = 0.0;
    std::optional<std::string> normalized_text;
    for (const auto& token : query.tokens) {
        if (token.phrase) {
            if (!normalized_text) {
                normalized_text = normalize_whitespace_lower(text);
            }
            const auto phrase = normalize_whitespace_lower(token.value);
            if (phrase.empty()) {
                continue;
            }
            const auto position = normalized_text->find(phrase);
            if (position == std::string::npos) {
                return {.matches = false, .score = 0.0};
            }
            total_score += static_cast<double>(position) * 0.1;
            continue;
        }
        const auto match = cch::tui::fuzzy_match(token.value, text);
        if (!match.matches) {
            return {.matches = false, .score = 0.0};
        }
        total_score += match.score;
    }
    return {.matches = true, .score = total_score};
}

std::vector<session_discovery::SessionInfo> filter_and_sort_sessions(
    std::vector<session_discovery::SessionInfo> sessions,
    std::string_view query,
    SessionSortMode sort_mode,
    SessionNameFilter name_filter) {
    if (name_filter == SessionNameFilter::Named) {
        std::erase_if(sessions, [](const session_discovery::SessionInfo& session) {
            return !has_session_name(session);
        });
    }
    const auto trimmed = trim_view(query);
    if (trimmed.empty()) {
        return sessions;
    }

    const auto parsed = parse_session_search_query(query);
    if (parsed.regex_error) {
        return {};
    }

    // Recent mode: filter only, keep the incoming (newest-first) order.
    if (sort_mode == SessionSortMode::Recent) {
        std::vector<session_discovery::SessionInfo> filtered;
        for (auto& session : sessions) {
            if (match_session(session, parsed).matches) {
                filtered.push_back(std::move(session));
            }
        }
        return filtered;
    }

    // Relevance mode: sort by score, tie-break by modified desc.
    struct Scored {
        session_discovery::SessionInfo session;
        double score{0.0};
    };
    std::vector<Scored> scored;
    for (auto& session : sessions) {
        const auto result = match_session(session, parsed);
        if (!result.matches) {
            continue;
        }
        scored.push_back(Scored{
            .session = std::move(session),
            .score = result.score,
        });
    }
    std::sort(scored.begin(), scored.end(), [](const Scored& first, const Scored& second) {
        if (first.score != second.score) {
            return first.score < second.score;
        }
        return first.session.modified > second.session.modified;
    });
    std::vector<session_discovery::SessionInfo> result;
    result.reserve(scored.size());
    for (auto& entry : scored) {
        result.push_back(std::move(entry.session));
    }
    return result;
}

} // namespace cch::coding_agent::tui
