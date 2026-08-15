#include "cli/ListModels.hpp"

#include <cch/ai/Model.hpp>
#include <cch/coding_agent/AuthGuidance.hpp>
#include <cch/tui/Fuzzy.hpp>
#include <cch/support/Error.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <iterator>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace cch::cli {
namespace {

/// pi `cli/list-models.ts` `formatTokenCount`, verbatim: 200000 -> "200K",
/// 1000000 -> "1M", fractional counts keep one decimal. JS `toFixed(1)`
/// rounds half away from zero (n/10^f picks the larger n), so the decimal
/// formatting rounds half up like pi.
[[nodiscard]] std::string format_token_count(std::uint64_t count) {
    if (count >= 1'000'000) {
        if (count % 1'000'000 == 0) {
            return std::format("{}M", count / 1'000'000);
        }
        const auto tenths =
            static_cast<std::uint64_t>(count / 100'000.0 + 0.5);
        return std::format("{}.{}M", tenths / 10, tenths % 10);
    }
    if (count >= 1'000) {
        if (count % 1'000 == 0) {
            return std::format("{}K", count / 1'000);
        }
        const auto tenths =
            static_cast<std::uint64_t>(count / 100.0 + 0.5);
        return std::format("{}.{}K", tenths / 10, tenths % 10);
    }
    return std::to_string(count);
}

struct ListModelsRow {
    std::string provider;
    std::string model;
    std::string context;
    std::string max_out;
    std::string thinking;
    std::string images;
};

[[nodiscard]] ListModelsRow make_row(const ai::Model& model) {
    const auto has_images =
        std::find(model.input.begin(), model.input.end(), ai::ModelInput::Image) !=
        model.input.end();
    return ListModelsRow{
        .provider = model.provider,
        .model = model.id,
        .context = format_token_count(model.context_window),
        .max_out = format_token_count(model.max_tokens),
        .thinking = model.reasoning ? "yes" : "no",
        .images = has_images ? "yes" : "no",
    };
}

[[nodiscard]] std::string pad(std::string text, std::size_t width) {
    if (text.size() < width) {
        text.append(width - text.size(), ' ');
    }
    return text;
}

[[nodiscard]] std::string join_row(
    const ListModelsRow& row,
    const std::array<std::size_t, 6>& widths) {
    return pad(row.provider, widths[0]) + "  " +
           pad(row.model, widths[1]) + "  " +
           pad(row.context, widths[2]) + "  " +
           pad(row.max_out, widths[3]) + "  " +
           pad(row.thinking, widths[4]) + "  " +
           pad(row.images, widths[5]);
}

} // namespace

void print_list_models(
    const coding_agent::ModelRuntime& runtime,
    const std::optional<std::string>& search,
    std::ostream& output,
    std::ostream& error) {
    // pi list-models.ts: the models.json load error warns on stderr first
    // (yellow in pi; the C++ CLI surfaces warnings without ANSI color).
    if (auto load_error = runtime.get_error()) {
        error << "Warning: errors loading models.json:\n" << *load_error << '\n';
    }

    // pi: `getAvailable` — only models whose provider has configured auth.
    auto models = runtime.get_available_snapshot();

    if (models.empty()) {
        output << coding_agent::format_no_models_available_message(
            std::filesystem::path{
                coding_agent::kDefaultAuthGuidanceDocsPath})
               << '\n';
        return;
    }

    // pi: fuzzy `[search]` filter over `${provider} ${id}`.
    if (search && !search->empty()) {
        models = cch::tui::fuzzy_filter(
            std::move(models),
            *search,
            [](const ai::Model& model) {
                return model.provider + " " + model.id;
            });
        if (models.empty()) {
            output << "No models matching \"" << *search << "\"\n";
            return;
        }
    }

    // pi: sort by provider, then by model id.
    std::stable_sort(
        models.begin(),
        models.end(),
        [](const ai::Model& left, const ai::Model& right) {
            if (left.provider != right.provider) {
                return left.provider < right.provider;
            }
            return left.id < right.id;
        });

    const auto rows = [&]() {
        std::vector<ListModelsRow> result;
        result.reserve(models.size());
        for (const auto& model : models) {
            result.push_back(make_row(model));
        }
        return result;
    }();

    const std::array<std::string, 6> headers{
        "provider", "model", "context", "max-out", "thinking", "images"};
    std::array<std::size_t, 6> widths{};
    for (std::size_t column = 0; column < 6; ++column) {
        widths[column] = headers[column].size();
        for (const auto& row : rows) {
            const auto& value = [&]() -> const std::string& {
                switch (column) {
                case 0: return row.provider;
                case 1: return row.model;
                case 2: return row.context;
                case 3: return row.max_out;
                case 4: return row.thinking;
                default: return row.images;
                }
            }();
            widths[column] = std::max(widths[column], value.size());
        }
    }

    output << join_row(
                  ListModelsRow{
                      .provider = headers[0],
                      .model = headers[1],
                      .context = headers[2],
                      .max_out = headers[3],
                      .thinking = headers[4],
                      .images = headers[5],
                  },
                  widths)
           << '\n';
    for (const auto& row : rows) {
        output << join_row(row, widths) << '\n';
    }
}

} // namespace cch::cli
