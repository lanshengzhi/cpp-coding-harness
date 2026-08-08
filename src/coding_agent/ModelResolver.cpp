// Model resolution and scoping (pi `core/model-resolver.ts` subset at the
// frozen baseline 83114817): exact reference matching, `parseModelPattern`
// with `:level` suffixes, and glob/plain pattern scoping with pi's
// diagnostics. This is the single pattern-resolution seam for `--models` and
// settings `enabledModels`; the CLI `--model` single-model path keeps its own
// provider-inference resolution in the session factory.

#include <cch/coding_agent/ModelResolver.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent {
namespace {

[[nodiscard]] std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

/// pi `isValidThinkingLevel`: one of the seven wire levels.
[[nodiscard]] bool is_valid_thinking_level(std::string_view level) {
    return level == "off" || level == "minimal" || level == "low" ||
        level == "medium" || level == "high" || level == "xhigh" ||
        level == "max";
}

/// pi `isAlias`: a model id with no date suffix (`-YYYYMMDD`); `-latest` ids
/// and undated ids are aliases.
[[nodiscard]] bool is_alias(std::string_view id) {
    if (id.ends_with("-latest")) return true;
    if (id.size() < 9) return true;
    const auto date = id.substr(id.size() - 9);
    if (date.front() != '-') return true;
    return !std::all_of(date.begin() + 1, date.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}

/// pi `tryMatchModel`: exact match first, then partial id/name matches with
/// alias preference over dated versions.
[[nodiscard]] std::optional<ai::Model> try_match_model(
    std::string_view model_pattern,
    const std::vector<ai::Model>& available_models) {
    if (auto exact = find_exact_model_reference_match(model_pattern, available_models)) {
        return exact;
    }

    const auto needle = lowercase(std::string{model_pattern});
    std::vector<const ai::Model*> matches;
    for (const auto& model : available_models) {
        if (lowercase(model.id).find(needle) != std::string::npos ||
            lowercase(model.name).find(needle) != std::string::npos) {
            matches.push_back(&model);
        }
    }
    if (matches.empty()) return std::nullopt;

    std::vector<const ai::Model*> aliases;
    std::vector<const ai::Model*> dated;
    for (const auto* model : matches) {
        (is_alias(model->id) ? aliases : dated).push_back(model);
    }
    if (!aliases.empty()) {
        std::sort(aliases.begin(), aliases.end(), [](const ai::Model* left, const ai::Model* right) {
            return left->id > right->id;
        });
        return *aliases.front();
    }
    std::sort(dated.begin(), dated.end(), [](const ai::Model* left, const ai::Model* right) {
        return left->id > right->id;
    });
    return *dated.front();
}

/// Minimatch subset (pi uses the `minimatch` library with defaults): `*`
/// matches any run of characters except `/`, `?` matches one character except
/// `/`, `[...]` is a character class (`[!...]`/`[^...]` negated), a run of
/// two or more stars (`**`) also crosses `/`, and matching is
/// case-insensitive for `nocase`. Whole-string matching like minimatch.
[[nodiscard]] bool glob_match(
    std::string_view pattern,
    std::string_view text,
    bool nocase) {
    const auto fold = [nocase](unsigned char ch) {
        return nocase ? static_cast<char>(std::tolower(ch)) : static_cast<char>(ch);
    };
    const auto char_class_matches = [&](std::size_t open, std::size_t close, char value) {
        bool negate = false;
        std::size_t cursor = open + 1;
        if (cursor < close && (pattern[cursor] == '!' || pattern[cursor] == '^')) {
            negate = true;
            ++cursor;
        }
        bool matched = false;
        while (cursor < close) {
            const char first = pattern[cursor];
            if (cursor + 2 < close && pattern[cursor + 1] == '-') {
                const char last = pattern[cursor + 2];
                for (char candidate = first;; ++candidate) {
                    if (fold(static_cast<unsigned char>(candidate)) ==
                        fold(static_cast<unsigned char>(value))) {
                        matched = true;
                        break;
                    }
                    if (candidate == last) break;
                }
                cursor += 3;
            } else {
                if (fold(static_cast<unsigned char>(first)) ==
                    fold(static_cast<unsigned char>(value))) {
                    matched = true;
                }
                ++cursor;
            }
        }
        return matched != negate;
    };

    // Memoized recursion over (pattern index, text index). `*` expands over
    // non-slash runs; `**` over any run; `?` over one non-slash character;
    // character classes never match `/`.
    std::vector<std::vector<std::int8_t>> memo(pattern.size() + 1, std::vector<std::int8_t>(text.size() + 1, -1));
    std::function<bool(std::size_t, std::size_t)> match =
        [&](std::size_t pattern_index, std::size_t text_index) -> bool {
            if (memo[pattern_index][text_index] != -1) {
                return memo[pattern_index][text_index] != 0;
            }
            const auto result = [&]() -> bool {
                if (pattern_index == pattern.size()) {
                    return text_index == text.size();
                }
                if (pattern[pattern_index] == '*') {
                    std::size_t end = pattern_index;
                    while (end < pattern.size() && pattern[end] == '*') ++end;
                    // minimatch globstar semantics: a run of two or more
                    // stars crosses `/` only as a full path segment
                    // (preceded by the pattern start or `/` and followed by
                    // the pattern end or `/`); otherwise it behaves like a
                    // single non-crossing `*`.
                    const bool globstar = end - pattern_index >= 2;
                    const bool segment_start =
                        pattern_index == 0 || pattern[pattern_index - 1] == '/';
                    const bool segment_end =
                        end == pattern.size() || pattern[end] == '/';
                    const bool crosses = globstar && segment_start && segment_end;
                    if (end == pattern.size()) return true;
                    if (crosses) {
                        for (std::size_t run = text_index; run <= text.size(); ++run) {
                            if (match(end, run)) return true;
                        }
                        return false;
                    }
                    for (std::size_t run = text_index; run <= text.size(); ++run) {
                        if (run > text_index && text[run - 1] == '/') break;
                        if (match(end, run)) return true;
                    }
                    return false;
                }
                if (text_index == text.size()) return false;
                if (pattern[pattern_index] == '?') {
                    if (text[text_index] == '/') return false;
                    return match(pattern_index + 1, text_index + 1);
                }
                if (pattern[pattern_index] == '[') {
                    const auto close = pattern.find(']', pattern_index + 1);
                    if (close != std::string_view::npos && text[text_index] != '/' &&
                        char_class_matches(pattern_index, close, text[text_index])) {
                        return match(close + 1, text_index + 1);
                    }
                    return false;
                }
                const char folded_pattern = fold(static_cast<unsigned char>(pattern[pattern_index]));
                const char folded_text = fold(static_cast<unsigned char>(text[text_index]));
                return folded_pattern == folded_text &&
                    match(pattern_index + 1, text_index + 1);
            }();
            memo[pattern_index][text_index] = result ? 1 : 0;
            return result;
        };
    return match(0, 0);
}

} // namespace

std::optional<ai::Model> find_exact_model_reference_match(
    std::string_view model_reference,
    const std::vector<ai::Model>& available_models) {
    const auto trimmed = [](std::string_view value) {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
            value.remove_prefix(1);
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
            value.remove_suffix(1);
        }
        return value;
    };
    const auto reference = trimmed(model_reference);
    if (reference.empty()) return std::nullopt;

    const auto normalized = lowercase(std::string{reference});

    const auto canonical_matches = [&] {
        std::vector<const ai::Model*> result;
        for (const auto& model : available_models) {
            if (lowercase(model.provider + "/" + model.id) == normalized) {
                result.push_back(&model);
            }
        }
        return result;
    }();
    if (canonical_matches.size() == 1) return *canonical_matches.front();
    if (canonical_matches.size() > 1) return std::nullopt;

    const auto slash = reference.find('/');
    if (slash != std::string_view::npos) {
        const auto provider = trimmed(reference.substr(0, slash));
        const auto model_id = trimmed(reference.substr(slash + 1));
        if (!provider.empty() && !model_id.empty()) {
            std::vector<const ai::Model*> provider_matches;
            for (const auto& model : available_models) {
                if (lowercase(model.provider) == lowercase(std::string{provider}) &&
                    lowercase(model.id) == lowercase(std::string{model_id})) {
                    provider_matches.push_back(&model);
                }
            }
            if (provider_matches.size() == 1) return *provider_matches.front();
            if (provider_matches.size() > 1) return std::nullopt;
        }
    }

    std::vector<const ai::Model*> id_matches;
    for (const auto& model : available_models) {
        if (lowercase(model.id) == normalized) {
            id_matches.push_back(&model);
        }
    }
    return id_matches.size() == 1 ? std::optional<ai::Model>{*id_matches.front()}
                                  : std::nullopt;
}

ParsedModelPattern parse_model_pattern(
    std::string_view pattern,
    const std::vector<ai::Model>& available_models,
    bool allow_invalid_thinking_level_fallback) {
    if (auto exact = try_match_model(pattern, available_models)) {
        return ParsedModelPattern{.model = std::move(exact)};
    }

    const auto last_colon = pattern.rfind(':');
    if (last_colon == std::string_view::npos) {
        return ParsedModelPattern{};
    }

    const auto prefix = pattern.substr(0, last_colon);
    const auto suffix = pattern.substr(last_colon + 1);
    if (is_valid_thinking_level(suffix)) {
        auto result = parse_model_pattern(
            prefix,
            available_models,
            allow_invalid_thinking_level_fallback);
        if (result.model) {
            // Only use this thinking level if no warning from inner recursion.
            result.thinking_level = result.warning ? std::nullopt : std::optional<std::string>{std::string{suffix}};
        }
        return result;
    }

    if (!allow_invalid_thinking_level_fallback) {
        // Strict CLI mode: treat the whole pattern as a model id and fail.
        return ParsedModelPattern{};
    }

    auto result = parse_model_pattern(
        prefix,
        available_models,
        allow_invalid_thinking_level_fallback);
    if (result.model) {
        result.warning = std::format(
            "Invalid thinking level \"{}\" in pattern \"{}\". Using default instead.",
            suffix,
            pattern);
        result.thinking_level = std::nullopt;
    }
    return result;
}

ModelScopeResolution resolve_model_scope_with_diagnostics(
    const std::vector<std::string>& patterns,
    const std::vector<ai::Model>& available_models) {
    ModelScopeResolution resolution;
    const auto already_scoped = [&](const ai::Model& model) {
        return std::any_of(
            resolution.scoped_models.begin(),
            resolution.scoped_models.end(),
            [&](const ScopedModel& scoped) {
                return scoped.model.provider == model.provider &&
                    scoped.model.id == model.id;
            });
    };

    for (const auto& pattern : patterns) {
        const bool has_glob = pattern.find('*') != std::string::npos ||
            pattern.find('?') != std::string::npos ||
            pattern.find('[') != std::string::npos;
        if (has_glob) {
            // Extract an optional thinking-level suffix (e.g.
            // `provider/*:high`).
            std::string glob_pattern = pattern;
            std::optional<std::string> thinking_level;
            const auto colon = pattern.rfind(':');
            if (colon != std::string::npos) {
                const auto suffix = std::string_view{pattern}.substr(colon + 1);
                if (is_valid_thinking_level(suffix)) {
                    thinking_level = std::string{suffix};
                    glob_pattern = pattern.substr(0, colon);
                }
            }

            if (auto exact = find_exact_model_reference_match(glob_pattern, available_models)) {
                if (!already_scoped(*exact)) {
                    resolution.scoped_models.push_back(
                        ScopedModel{.model = *exact, .thinking_level = thinking_level});
                }
                continue;
            }

            // Match against `provider/modelId` OR just the model id (pi
            // minimatch with `{nocase: true}`).
            std::vector<const ai::Model*> matching;
            for (const auto& model : available_models) {
                if (glob_match(glob_pattern, model.provider + "/" + model.id, /* nocase */ true) ||
                    glob_match(glob_pattern, model.id, /* nocase */ true)) {
                    matching.push_back(&model);
                }
            }
            if (matching.empty()) {
                resolution.diagnostics.push_back(ModelScopeDiagnostic{
                    .code = "no-match",
                    .message = "No models match pattern \"" + pattern + "\"",
                    .pattern = pattern,
                });
                continue;
            }
            for (const auto* model : matching) {
                if (!already_scoped(*model)) {
                    resolution.scoped_models.push_back(
                        ScopedModel{.model = *model, .thinking_level = thinking_level});
                }
            }
            continue;
        }

        const auto parsed = parse_model_pattern(pattern, available_models);
        if (parsed.warning) {
            resolution.diagnostics.push_back(ModelScopeDiagnostic{
                .code = "invalid-thinking-level",
                .message = *parsed.warning,
                .pattern = pattern,
            });
        }
        if (!parsed.model) {
            resolution.diagnostics.push_back(ModelScopeDiagnostic{
                .code = "no-match",
                .message = "No models match pattern \"" + pattern + "\"",
                .pattern = pattern,
            });
            continue;
        }
        if (!already_scoped(*parsed.model)) {
            resolution.scoped_models.push_back(ScopedModel{
                .model = *parsed.model,
                .thinking_level = parsed.thinking_level,
            });
        }
    }
    return resolution;
}

std::vector<ScopedModel> resolve_model_scope(
    const std::vector<std::string>& patterns,
    const std::vector<ai::Model>& available_models) {
    return resolve_model_scope_with_diagnostics(patterns, available_models).scoped_models;
}

} // namespace cch::coding_agent
