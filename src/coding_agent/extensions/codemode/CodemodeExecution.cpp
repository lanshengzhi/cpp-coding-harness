#include "CodemodeExecution.hpp"

#include "coding_agent/Base64.hpp"
#include "coding_agent/extensions/codemode/CodemodeTool.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/Json.hpp"
#include "support/RandomHex.hpp"

#include <cch/ai/Content.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/OutputLimiter.hpp>

#include <boost/asio/awaitable.hpp>
#include <utf8proc.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::coding_agent::extensions {

namespace {

using support::JsonValue;

/// pi `ARGS_PREVIEW_CHARS` / `ERROR_PREVIEW_CHARS`.
constexpr std::size_t kArgsPreviewChars = 200;
constexpr std::size_t kErrorPreviewChars = 500;
/// pi `DEFAULT_MAX_OUTPUT_TOKENS` (`extensions/codemode/execute.ts`).
constexpr std::int64_t kDefaultMaxOutputTokens = 10'000;
/// pi `CHARS_PER_TOKEN`: the estimator that turns a token budget into a
/// character budget.
constexpr double kCharsPerToken = 4.0;

/// pi's JS strings count and slice UTF-16 code units, so every budget and
/// preview length is measured in them rather than UTF-8 bytes. Invalid UTF-8
/// becomes U+FFFD, matching the decoding a JS string performs.
[[nodiscard]] std::u16string to_utf16(std::string_view text) {
    std::u16string units;
    units.reserve(text.size());
    for (std::size_t offset = 0; offset < text.size();) {
        utf8proc_int32_t codepoint{0};
        const auto decoded = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t*>(text.data()) + offset,
                static_cast<utf8proc_ssize_t>(text.size() - offset),
                &codepoint);
        if (decoded <= 0) {
            units.push_back(static_cast<char16_t>(0xfffd));
            ++offset;
            continue;
        }
        offset += static_cast<std::size_t>(decoded);
        if (codepoint >= 0x10000) {
            const auto adjusted = codepoint - 0x10000;
            units.push_back(static_cast<char16_t>(0xd800 + (adjusted >> 10)));
            units.push_back(static_cast<char16_t>(0xdc00 + (adjusted & 0x3ff)));
        } else {
            units.push_back(static_cast<char16_t>(codepoint));
        }
    }
    return units;
}

[[nodiscard]] std::string to_utf8(std::u16string_view units) {
    std::string text;
    text.reserve(units.size());
    for (std::size_t index = 0; index < units.size(); ++index) {
        utf8proc_int32_t codepoint = units[index];
        if (codepoint >= 0xd800 && codepoint <= 0xdbff && index + 1 < units.size() && units[index + 1] >= 0xdc00 &&
                units[index + 1] <= 0xdfff) {
            codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (units[index + 1] - 0xdc00);
            ++index;
        } else if (codepoint >= 0xd800 && codepoint <= 0xdfff) {
            codepoint = 0xfffd;
        }
        utf8proc_uint8_t encoded[4]{};
        const auto written = utf8proc_encode_char(codepoint, encoded);
        text.append(reinterpret_cast<const char*>(encoded), static_cast<std::size_t>(written));
    }
    return text;
}

/// pi `truncateText`: cut to `maxChars` UTF-16 units, ending in `...`.
[[nodiscard]] std::string truncate_text(std::string_view text, std::size_t max_chars) {
    const auto units = to_utf16(text);
    if (units.size() <= max_chars) return std::string{text};
    return to_utf8(units.substr(0, max_chars - 3)) + "...";
}

[[nodiscard]] std::string wall_time_seconds(std::chrono::steady_clock::duration elapsed) {
    return std::format("{:.1f}", std::chrono::duration<double>(elapsed).count());
}

/// pi `formatCallSummary`.
[[nodiscard]] std::string format_call_summary(const std::vector<CodemodeCall>& calls) {
    if (calls.empty()) return "No tool calls were made.";
    std::string summary = "Tool calls made before the failure (they are not undone): ";
    for (std::size_t index = 0; index < calls.size(); ++index) {
        if (index > 0) summary += ", ";
        summary += calls[index].name + " (" + calls[index].status + ")";
    }
    return summary;
}

/// pi `formatError`.
[[nodiscard]] std::string format_error(const codemode::CodemodeError& error, const std::vector<CodemodeCall>& calls) {
    std::string head;
    switch (error.kind) {
    case codemode::CodemodeError::Kind::Script:
        head = error.stack.has_value() ? *error.stack : (error.name.value_or("Error") + ": " + error.message);
        break;
    case codemode::CodemodeError::Kind::Timeout:
        head = "Script timed out: " + error.message;
        break;
    case codemode::CodemodeError::Kind::Aborted:
        head = "Script aborted: " + error.message;
        break;
    case codemode::CodemodeError::Kind::Sandbox:
        head = "Script sandbox failed: " + error.message;
        break;
    }
    return head + "\n\n" + format_call_summary(calls);
}

/// pi `valueText`: strings as they are, other values as compact JSON.
[[nodiscard]] std::string value_text(std::string_view value_json) {
    if (auto parsed = support::read_json(value_json); parsed && parsed->holds<std::string>()) {
        return parsed->get_string();
    }
    return std::string{value_json};
}

[[nodiscard]] bool is_text(const codemode::CodemodeOutputItem& item) {
    return item.kind == codemode::CodemodeOutputItem::Kind::Text;
}

/// pi `formatOutput`: number the script's text items when there is more than
/// one, and place every `console.*` line after the other output in one block.
[[nodiscard]] std::vector<codemode::CodemodeOutputItem> format_output(
        const std::vector<codemode::CodemodeOutputItem>& items) {
    std::size_t total = 0;
    for (const auto& item : items) {
        if (is_text(item) && !item.console) ++total;
    }
    std::vector<codemode::CodemodeOutputItem> formatted;
    formatted.reserve(items.size());
    std::vector<std::string> console_lines;
    std::size_t index = 0;
    for (const auto& item : items) {
        if (!is_text(item)) {
            formatted.push_back(item);
            continue;
        }
        if (item.console) {
            console_lines.push_back(item.data);
            continue;
        }
        ++index;
        codemode::CodemodeOutputItem text = item;
        if (total > 1) {
            text.data = std::format("==> text {}/{} <==\n{}", index, total, item.data);
        }
        formatted.push_back(std::move(text));
    }
    if (!console_lines.empty()) {
        std::string block = "<console_output>\n";
        for (std::size_t line = 0; line < console_lines.size(); ++line) {
            if (line > 0) block += "\n";
            block += console_lines[line];
        }
        block += "\n</console_output>";
        formatted.push_back(codemode::CodemodeOutputItem{
                .kind = codemode::CodemodeOutputItem::Kind::Text,
                .data = std::move(block),
                .mime_type = {},
                .console = false,
        });
    }
    return formatted;
}

/// pi `joinAdjacentText`: one text item per run of adjacent text, each part
/// starting on its own line. Images stay separate and flush the run.
[[nodiscard]] std::vector<ai::Content> join_adjacent_text(const std::vector<codemode::CodemodeOutputItem>& items) {
    std::vector<ai::Content> joined;
    std::string parts;
    std::string last_part;
    const auto flush = [&]() {
        if (!parts.empty()) {
            joined.push_back(ai::text_content(std::move(parts)));
            parts.clear();
        }
        last_part.clear();
    };
    for (const auto& item : items) {
        if (!is_text(item)) {
            flush();
            joined.push_back(ai::image_content(item.data, item.mime_type));
            continue;
        }
        if (!last_part.empty() && last_part.back() != '\n') {
            parts += '\n';
            last_part = "\n";
        }
        parts += item.data;
        if (!item.data.empty()) last_part = item.data;
    }
    flush();
    return joined;
}

/// pi `IMAGE_EXTENSIONS`.
[[nodiscard]] std::optional<std::string_view> image_extension(std::string_view mime_type) {
    if (mime_type == "image/png") return ".png";
    if (mime_type == "image/jpeg") return ".jpg";
    if (mime_type == "image/gif") return ".gif";
    if (mime_type == "image/webp") return ".webp";
    return std::nullopt;
}

/// pi `createOutputFilePath`: a fresh path in the system temporary directory.
[[nodiscard]] std::filesystem::path output_file_path(std::string_view extension) {
    std::error_code error;
    const auto directory = std::filesystem::temp_directory_path(error);
    const auto root = error ? std::filesystem::path{"."} : directory;
    return root / ("pi-codemode-" + support::random_hex_id(8) + std::string{extension});
}

/// pi `writeOutputFile`: write one fresh output file and name it, or report why
/// the write failed so the caller can keep the script's result. `session` must
/// outlive the returned operation; the tool call owns it for the whole run.
[[nodiscard]] boost::asio::awaitable<support::Expected<std::string>> write_output_file(
        std::shared_ptr<CodemodeSessionState> session,
        std::string extension,
        harness::WriteContent content,
        std::stop_token stop_token) {
    if (session == nullptr || !session->filesystem) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Workspace, "output files are unavailable"));
    }
    const auto path = output_file_path(extension);
    auto written = co_await support::detail::await_async_result(
            session->filesystem->writeFile(path.string(), std::move(content), stop_token));
    if (!written) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Workspace, written.error().message));
    }
    co_return support::Expected<std::string>{path.string()};
}

/// pi `saveImages`: write each image's bytes once and put a path label before
/// it. A failed write keeps the script's result and becomes part of the label.
[[nodiscard]] boost::asio::awaitable<std::vector<ai::Content>> save_images(
        std::vector<ai::Content> items, std::shared_ptr<CodemodeSessionState> session, std::stop_token stop_token) {
    std::map<std::string, std::string> labels;
    std::vector<ai::Content> saved;
    saved.reserve(items.size());
    for (const auto& item : items) {
        const auto* image = std::get_if<ai::ImageContent>(&item);
        if (image == nullptr) {
            saved.push_back(item);
            continue;
        }
        auto found = labels.find(image->data);
        if (found == labels.end()) {
            const auto bytes = decode_base64(image->data);
            const std::string kind = image->mime_type + ", " + support::format_output_size(bytes.size());
            const auto extension = image_extension(image->mime_type);
            std::string label;
            if (!extension.has_value()) {
                label = std::format(
                        "[Image ({}) could not be saved: No file extension for image type {}]", kind, image->mime_type);
            } else {
                harness::BinaryData content;
                content.reserve(bytes.size());
                for (const auto byte : bytes) {
                    content.push_back(static_cast<std::byte>(byte));
                }
                auto written =
                        co_await write_output_file(session, std::string{*extension}, std::move(content), stop_token);
                label = written ? std::format("[Image saved to {} ({})]", *written, kind)
                                : std::format("[Image ({}) could not be saved: {}]", kind, written.error().message);
            }
            found = labels.emplace(image->data, std::move(label)).first;
        }
        saved.push_back(ai::text_content(found->second));
        saved.push_back(item);
    }
    co_return saved;
}

/// pi `truncateOutput`: over budget, keep the start and end of the combined
/// text, move the images after it, and spill the full text to a file.
struct TruncatedOutput {
    std::vector<ai::Content> items;
    std::optional<std::string> full_output_path;
};

[[nodiscard]] boost::asio::awaitable<TruncatedOutput> truncate_output(std::vector<ai::Content> items,
        std::int64_t max_tokens,
        std::shared_ptr<CodemodeSessionState> session,
        std::stop_token stop_token) {
    std::vector<std::string> texts;
    for (const auto& item : items) {
        if (const auto* text = std::get_if<ai::TextContent>(&item); text != nullptr) texts.push_back(text->text);
    }
    std::string combined;
    for (std::size_t index = 0; index < texts.size(); ++index) {
        if (index > 0) combined += '\n';
        combined += texts[index];
    }
    const auto units = to_utf16(combined);
    const double budget = static_cast<double>(max_tokens) * kCharsPerToken;
    if (texts.empty() || static_cast<double>(units.size()) <= budget) {
        co_return TruncatedOutput{.items = std::move(items), .full_output_path = std::nullopt};
    }
    const std::size_t head_chars = static_cast<std::size_t>(budget / 2.0);
    const std::size_t tail_chars = static_cast<std::size_t>(budget) - head_chars;
    const std::size_t removed = units.size() - head_chars - tail_chars;
    const std::size_t total_tokens = (units.size() + 3) / 4;
    const std::size_t removed_tokens = (removed + 3) / 4;
    const std::size_t total_lines = 1 + static_cast<std::size_t>(std::count(combined.begin(), combined.end(), '\n'));
    std::string text =
            std::format("Warning: truncated output (original token count: {})\nTotal output lines: {}\n\n{}…{} tokens "
                        "truncated…{}",
                    total_tokens,
                    total_lines,
                    to_utf8(units.substr(0, head_chars)),
                    removed_tokens,
                    tail_chars > 0 ? to_utf8(units.substr(units.size() - tail_chars)) : std::string{});
    TruncatedOutput truncated;
    auto spilled = co_await write_output_file(std::move(session), ".txt", std::move(combined), stop_token);
    if (spilled) {
        text += std::format("\n\n[Full output: {} (read with offset/limit)]", *spilled);
        truncated.full_output_path = *spilled;
    } else {
        text += std::format("\n\n[Could not save the full output: {}]", spilled.error().message);
    }
    truncated.items.push_back(ai::text_content(std::move(text)));
    for (auto& item : items) {
        if (std::holds_alternative<ai::ImageContent>(item)) truncated.items.push_back(std::move(item));
    }
    co_return truncated;
}

[[nodiscard]] JsonValue calls_json(const std::vector<CodemodeCall>& calls) {
    JsonValue::array_t records;
    records.reserve(calls.size());
    for (const auto& call : calls) {
        JsonValue::object_t record{
                {"id", call.id},
                {"name", call.name},
                {"args", call.args},
                {"status", call.status},
        };
        if (call.duration_ms.has_value()) record.emplace("durationMs", *call.duration_ms);
        if (call.error.has_value()) record.emplace("error", *call.error);
        records.emplace_back(std::move(record));
    }
    return JsonValue{std::move(records)};
}

/// pi `appendEntry(CODEMODE_STORE_ENTRY_TYPE, { set, delete })`: persist a
/// successful run's write set before the tool result reports the run.
[[nodiscard]] support::ExpectedVoid persist_store_writes(
        CodemodeSessionState& session, const codemode::CodemodeStoreWrites& writes) {
    if (writes.set.empty() && writes.deleted.empty()) return {};
    JsonValue::object_t set;
    for (const auto& [key, value] : writes.set)
        set.emplace(key, value);
    JsonValue::array_t deleted;
    deleted.reserve(writes.deleted.size());
    for (const auto& key : writes.deleted)
        deleted.emplace_back(key);
    JsonValue data{JsonValue::object_t{
            {"set", JsonValue{std::move(set)}},
            {"delete", JsonValue{std::move(deleted)}},
    }};
    return session.append_store(std::string{kCodemodeStoreEntryType}, std::move(data));
}

} // namespace

std::map<std::string, JsonValue> codemode_store_from_branch(const std::vector<harness::session::SessionEntry>& branch) {
    std::map<std::string, JsonValue> store;
    for (auto entry = branch.rbegin(); entry != branch.rend(); ++entry) {
        if (entry->kind != harness::session::SessionEntryKind::Custom) continue;
        const auto* custom = std::get_if<harness::session::CustomEntryValue>(&entry->value);
        if (custom == nullptr || custom->custom_type != kCodemodeStoreEntryType || !custom->data.has_value()) continue;
        const auto* fields = custom->data->get_if<JsonValue::object_t>();
        if (fields == nullptr) continue;
        const auto set = fields->find("set");
        const auto deleted = fields->find("delete");
        if (set == fields->end() || deleted == fields->end() || !deleted->second.holds<JsonValue::array_t>()) continue;
        for (const auto& key : deleted->second.get_array()) {
            if (key.holds<std::string>()) store.erase(key.get_string());
        }
        if (const auto* values = set->second.get_if<JsonValue::object_t>(); values != nullptr) {
            for (const auto& [key, value] : *values)
                store.insert_or_assign(key, value);
        }
    }
    return store;
}

std::string codemode_preview_args(std::string_view arguments_json) {
    return arguments_json.empty() ? std::string{} : truncate_text(arguments_json, kArgsPreviewChars);
}

std::string codemode_error_preview(std::string_view error_text, std::string_view tool_name) {
    if (error_text.empty()) {
        return truncate_text(std::format("Tool \"{}\" failed", tool_name), kErrorPreviewChars);
    }
    return truncate_text(error_text, kErrorPreviewChars);
}

support::AsyncResult<ExtensionToolResult> present_codemode_run(const codemode::CodemodeRunResult& run,
        std::vector<CodemodeCall> calls,
        const codemode::CodemodeSourceOptions& options,
        std::chrono::steady_clock::duration elapsed,
        std::shared_ptr<CodemodeSessionState> session,
        std::stop_token stop_token) {
    // Calls still marked running were cut off by the script ending, a timeout,
    // or an abort (pi's post-run pass).
    for (auto& call : calls) {
        if (call.status == "running") call.status = "cancelled";
    }
    // pi persists a successful run's write set before formatting the result; a
    // failed run reports no writes and leaves the stored values untouched.
    if (!run.error.has_value() && run.store_writes.has_value() && session != nullptr && session->persistent()) {
        if (auto persisted = persist_store_writes(*session, *run.store_writes); !persisted) {
            return support::AsyncResult<ExtensionToolResult>{
                    support::Expected<ExtensionToolResult>{std::unexpected(std::move(persisted.error()))}};
        }
    }

    std::vector<codemode::CodemodeOutputItem> script_output = run.output;
    if (!run.error.has_value() && run.value_json.has_value()) {
        script_output.push_back(codemode::CodemodeOutputItem{
                .kind = codemode::CodemodeOutputItem::Kind::Text,
                .data = value_text(*run.value_json),
                .mime_type = {},
                .console = false,
        });
    }
    auto formatted = format_output(script_output);
    if (run.error.has_value()) {
        formatted.push_back(codemode::CodemodeOutputItem{
                .kind = codemode::CodemodeOutputItem::Kind::Text,
                .data = "Script error:\n" + format_error(*run.error, calls),
                .mime_type = {},
                .console = false,
        });
    }
    const std::int64_t max_tokens = options.max_output_tokens.value_or(kDefaultMaxOutputTokens);
    const bool is_error = run.error.has_value();
    const std::string header = std::string{is_error ? "Script failed" : "Script completed"} + "\nWall time " +
                               wall_time_seconds(elapsed) + " seconds\nOutput:\n";
    return support::detail::make_async_result(
            [joined = join_adjacent_text(formatted),
                    calls = std::move(calls),
                    max_tokens,
                    session,
                    stop_token,
                    header,
                    is_error]() mutable -> boost::asio::awaitable<support::Expected<ExtensionToolResult>> {
                auto truncated = co_await truncate_output(std::move(joined), max_tokens, session, stop_token);
                auto output = co_await save_images(std::move(truncated.items), session, stop_token);
                ExtensionToolResult result;
                result.is_error = is_error;
                result.content.push_back(ai::text_content(std::move(header)));
                for (auto& item : output)
                    result.content.push_back(std::move(item));
                JsonValue::object_t details{
                        {"calls", calls_json(calls)},
                };
                if (truncated.full_output_path.has_value()) {
                    details.emplace("fullOutputPath", *truncated.full_output_path);
                }
                result.details = JsonValue{std::move(details)};
                co_return support::Expected<ExtensionToolResult>{std::move(result)};
            });
}

} // namespace cch::coding_agent::extensions
