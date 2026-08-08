#include "coding_agent/tui/Footer.hpp"

#include <cch/tui/Utils.hpp>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace cch::coding_agent::tui {

std::string format_tokens(std::size_t count) {
    // pi formatTokens: thresholds at 1k/10k/1M/10M with one decimal below
    // the round boundary.
    if (count < 1000) return std::to_string(count);
    if (count < 10000) {
        return std::format("{:.1f}k", static_cast<double>(count) / 1000.0);
    }
    if (count < 1000000) return std::format("{}k", std::llround(count / 1000.0));
    if (count < 10000000) {
        return std::format("{:.1f}M", static_cast<double>(count) / 1000000.0);
    }
    return std::format("{}M", std::llround(count / 1000000.0));
}

std::string format_cwd_for_footer(
    const std::filesystem::path& cwd,
    const std::optional<std::filesystem::path>& home) {
    if (!home) return cwd.string();
    const auto resolved_cwd = std::filesystem::absolute(cwd);
    const auto resolved_home = std::filesystem::absolute(*home);
    std::error_code relative_error;
    const auto relative_to_home = std::filesystem::relative(
        resolved_cwd, resolved_home, relative_error);
    if (relative_error) return cwd.string();
    // pi isInsideHome: relative is "" (cwd == home), not "..", and not
    // starting with "../" and not absolute. `std::filesystem::relative`
    // returns "." for equal paths where Node returns "".
    const auto text = relative_to_home == "." ? "" : relative_to_home.string();
    const bool inside_home =
        text.empty() ||
        (text != ".." &&
         !text.starts_with(".." + std::string{std::filesystem::path::preferred_separator}) &&
         !relative_to_home.is_absolute());
    if (!inside_home) return cwd.string();
    return text.empty() ? "~" : "~" + std::string{std::filesystem::path::preferred_separator} + text;
}

util::Expected<cch::tui::RenderResult> Footer::render(std::size_t width) {
    if (width == 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Footer requires a positive visible width"));
    }

    // Line 1: dim pwd with the git branch suffix (pi footer.ts; the session
    // name suffix stays out of the subset — the runtime carries no name
    // surface, and the acceptance criteria feed the footer from
    // cwd/git-branch/provider data). Home is pi's
    // `process.env.HOME || process.env.USERPROFILE`.
    const char* home_env = std::getenv("HOME");
    if (home_env == nullptr || home_env[0] == '\0') {
        home_env = std::getenv("USERPROFILE");
    }
    auto pwd = format_cwd_for_footer(
        data_.cwd,
        home_env != nullptr && home_env[0] != '\0'
            ? std::optional<std::filesystem::path>{home_env}
            : std::nullopt);
    if (data_.git_branch) {
        pwd += " (" + *data_.git_branch + ")";
    }

    // Stats line parts in pi's order: input/output, cache reads/writes, the
    // cache hit rate, the cost (with the subscription marker), and the
    // context usage with its auto-compaction suffix.
    std::vector<std::string> stats_parts;
    if (data_.input) stats_parts.push_back("\xe2\x86\x91" + format_tokens(data_.input));
    if (data_.output) stats_parts.push_back("\xe2\x86\x93" + format_tokens(data_.output));
    if (data_.cache_read) stats_parts.push_back("R" + format_tokens(data_.cache_read));
    if (data_.cache_write) stats_parts.push_back("W" + format_tokens(data_.cache_write));
    if ((data_.cache_read > 0 || data_.cache_write > 0) && data_.cache_hit_rate) {
        stats_parts.push_back(std::format("CH{:.1f}%", *data_.cache_hit_rate));
    }
    if (data_.cost > 0 || data_.using_subscription) {
        stats_parts.push_back(std::format(
            "${:.3f}{}",
            data_.cost,
            data_.using_subscription ? " (sub)" : ""));
    }

    const auto auto_indicator = data_.auto_compact_enabled ? " (auto)" : "";
    const auto window_text = format_tokens(data_.context_window);
    std::string context_display;
    if (!data_.context_tokens) {
        context_display = "?/" + window_text + auto_indicator;
    } else {
        const double percent =
            data_.context_window > 0
                ? (static_cast<double>(*data_.context_tokens) /
                   static_cast<double>(data_.context_window)) * 100.0
                : 0.0;
        context_display =
            std::format("{:.1f}%/{}{}", percent, window_text, auto_indicator);
        if (percent > 90) {
            context_display = theme_.foreground(ThemeToken::Error, context_display);
        } else if (percent > 70) {
            context_display = theme_.foreground(ThemeToken::Warning, context_display);
        }
    }
    stats_parts.push_back(std::move(context_display));

    std::string stats_left;
    for (std::size_t index = 0; index < stats_parts.size(); ++index) {
        if (index > 0) stats_left += " ";
        stats_left += stats_parts[index];
    }
    auto stats_left_width = cch::tui::visible_width(stats_left);
    if (stats_left_width > width) {
        auto truncated = cch::tui::truncate_text(stats_left, width, "...");
        if (!truncated) return std::unexpected(truncated.error());
        stats_left = std::move(*truncated);
        stats_left_width = cch::tui::visible_width(stats_left);
    }

    // Right side: the model name, the thinking level when the model supports
    // reasoning, and the provider prefix when more than one provider is
    // available (pi's fallback drops the provider prefix when it would not
    // fit with the minimum two-space padding).
    const auto& model_name = data_.model_id.empty() ? "no-model" : data_.model_id;
    std::string right_side_without_provider = model_name;
    if (data_.model_reasoning) {
        right_side_without_provider =
            data_.thinking_level == "off"
                ? model_name + " \xc2\xb7 thinking off"
                : model_name + " \xc2\xb7 " + data_.thinking_level;
    }
    std::string right_side = right_side_without_provider;
    if (data_.available_provider_count > 1 && !data_.provider.empty()) {
        const auto prefixed = std::format(
            "({}) {}", data_.provider, right_side_without_provider);
        constexpr std::size_t kMinPadding = 2;
        if (stats_left_width + kMinPadding + cch::tui::visible_width(prefixed) <=
            width) {
            right_side = prefixed;
        }
    }
    const auto right_side_width = cch::tui::visible_width(right_side);
    const auto total_needed = stats_left_width + 2 + right_side_width;

    std::string stats_line;
    if (total_needed <= width) {
        stats_line = stats_left +
            std::string(width - stats_left_width - right_side_width, ' ') +
            right_side;
    } else {
        const auto available_for_right =
            width > stats_left_width + 2 ? width - stats_left_width - 2 : 0;
        if (available_for_right > 0) {
            auto truncated_right = cch::tui::truncate_text(
                right_side, available_for_right, "");
            if (!truncated_right) return std::unexpected(truncated_right.error());
            const auto truncated_width = cch::tui::visible_width(*truncated_right);
            stats_line = stats_left +
                std::string(
                    width > stats_left_width + truncated_width
                        ? width - stats_left_width - truncated_width
                        : 0,
                    ' ') +
                *truncated_right;
        } else {
            stats_line = stats_left;
        }
    }

    // pi applies dim to the stats-left and the remainder separately because
    // the colored context segment resets any surrounding dim wrapper.
    const auto dim_stats_left = theme_.foreground(ThemeToken::Dim, stats_left);
    const auto remainder = stats_line.substr(stats_left.size());
    const auto dim_remainder = theme_.foreground(ThemeToken::Dim, remainder);

    auto pwd_line = cch::tui::truncate_text(pwd, width, "...");
    if (!pwd_line) return std::unexpected(pwd_line.error());
    // pi dims the whole truncated pwd (dim + dim ellipsis); the C++
    // truncation runs on the plain text first because the truncation
    // machinery requires a plain ellipsis, and dimming is width-neutral.
    pwd_line = theme_.foreground(ThemeToken::Dim, std::move(*pwd_line));

    return cch::tui::RenderResult{
        .lines = {std::move(*pwd_line), dim_stats_left + dim_remainder},
    };
}

} // namespace cch::coding_agent::tui
