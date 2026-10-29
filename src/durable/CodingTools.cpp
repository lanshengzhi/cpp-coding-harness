#include <cch/durable/Environment.hpp>
#include <cch/durable/Images.hpp>
#include <cch/ai/Content.hpp>
#include <cch/support/OutputLimiter.hpp>

#include <algorithm>
#include <chrono>
#include <format>

namespace cch::durable {
namespace {
using Result = support::Expected<std::vector<ai::Content>>;
using PreparedResult = support::Expected<PreparedImage>;

support::Error tool_error(std::string text) { return support::make_error(support::ErrorCode::Tool, std::move(text)); }
support::Error image_error(std::string text) {
    return tool_error(std::format("<harness>\n[error] {}\n</harness>", text));
}
Result failure(std::string text) { return std::unexpected(tool_error(std::move(text))); }
Result text_result(std::string text) { return std::vector<ai::Content>{ai::text_content(std::move(text))}; }

/// An image the read tool found in a file: its path for diagnostics, its
/// bytes and their detected MIME type.
struct ReadImage {
    std::string_view path;
    std::span<const std::uint8_t> bytes;
    std::string_view mime_type;
};

PreparedResult prepare_with_processor(const ReadImage& image, const CodingToolOptions& options) {
    auto prepared = options.images->prepare(image.bytes, image.mime_type, options.limits);
    if (!prepared) {
        return std::unexpected(image_error(
                std::format("{} is an image ({}) that cannot be prepared for the model", image.path, image.mime_type)));
    }
    return std::move(*prepared);
}

PreparedResult prepare_without_processor(const ReadImage& image, const CodingToolOptions& options) {
    if (!is_inline_image_type(image.mime_type)) {
        return std::unexpected(image_error(
                std::format("{} is an image ({}) that needs converting, and no image processor is configured",
                        image.path,
                        image.mime_type)));
    }
    if (base64_encoded_length(image.bytes.size()) > options.limits.max_base64_bytes) {
        return std::unexpected(image_error(std::format("{} is an image ({}) of {}, too large to send, "
                                                       "and no image processor is configured to shrink it",
                image.path,
                image.mime_type,
                support::format_output_size(image.bytes.size()))));
    }
    const auto dimensions = image_dimensions(image.bytes, image.mime_type);
    if (!dimensions) {
        return std::unexpected(
                image_error(std::format("{} is an image ({}) whose size cannot be read", image.path, image.mime_type)));
    }
    if (dimensions->width > options.limits.max_width || dimensions->height > options.limits.max_height) {
        return std::unexpected(image_error(std::format("{} is an image ({}) of {}x{}, larger than {}x{}, "
                                                       "and no image processor is configured to shrink it",
                image.path,
                image.mime_type,
                dimensions->width,
                dimensions->height,
                options.limits.max_width,
                options.limits.max_height)));
    }
    return PreparedImage{.data = encode_base64(image.bytes), .mime_type = std::string{image.mime_type}};
}

std::vector<std::string> image_notes(const PreparedImage& image, bool vision) {
    std::vector<std::string> notes;
    if (image.converted_from)
        notes.push_back(std::format("Converted from {} to {}.", *image.converted_from, image.mime_type));
    if (image.resized) {
        const auto& [from, to] = *image.resized;
        notes.push_back(
                std::format("Resized from {}x{} to {}x{}. Multiply coordinates by {:.2f} to map them to the original.",
                        from.width,
                        from.height,
                        to.width,
                        to.height,
                        static_cast<double>(from.width) / static_cast<double>(to.width)));
    }
    if (!vision) notes.emplace_back("The current model does not support images; it sees a placeholder instead.");
    return notes;
}

std::vector<ai::Content> image_result(PreparedImage image, bool vision) {
    std::string message = std::format("Read image file [{}].", image.mime_type);
    for (const auto& note : image_notes(image, vision))
        message += std::format(" {}", note);
    return {ai::image_content(std::move(image.data), image.mime_type),
            ai::text_content(std::format("<harness>\n[info] {}\n</harness>", message))};
}

Result read_path(std::string_view path, Environment& env, const CodingToolOptions& options, std::stop_token stop) {
    auto content = env.read(path, stop);
    if (!content) return std::unexpected(content.error());
    const std::span<const std::uint8_t> bytes{reinterpret_cast<const std::uint8_t*>(content->data()), content->size()};
    const auto mime_type = detect_image_mime_type(bytes);
    if (!mime_type) return text_result(std::move(*content));

    const ReadImage image{.path = path, .bytes = bytes, .mime_type = *mime_type};
    auto prepared = options.images ? prepare_with_processor(image, options) : prepare_without_processor(image, options);
    if (!prepared) return std::unexpected(prepared.error());
    return image_result(std::move(*prepared), options.vision);
}
const support::JsonValue* field(const support::JsonValue& value, std::string_view key) {
    const auto* object = value.get_if<support::JsonValue::object_t>();
    if (!object) return nullptr;
    auto found = object->find(std::string{key});
    return found == object->end() ? nullptr : &found->second;
}
const std::string* string_field(const support::JsonValue& value, std::string_view key) {
    auto* item = field(value, key);
    return item ? item->get_if<std::string>() : nullptr;
}
Result run(std::string_view name,
        const support::JsonValue& args,
        Environment& env,
        const CodingToolOptions& options,
        std::stop_token stop) {
    if (name == "bash") {
        const auto* command = string_field(args, "command");
        if (!command) return failure("Invalid command");
        std::chrono::milliseconds timeout{0};
        if (auto* value = field(args, "timeout"); value && value->holds<double>() && value->get_number() > 0)
            timeout = std::chrono::milliseconds{static_cast<std::int64_t>(value->get_number() * 1000)};
        auto result = env.exec(*command, timeout, {}, stop);
        if (!result) return std::unexpected(result.error());
        if (result->exit_code != 0)
            return failure(result->output + std::format("\nCommand exited with code {}", result->exit_code));
        return text_result(std::move(result->output));
    }
    const auto* path = string_field(args, "path");
    if (!path) return failure("Invalid path");
    if (name == "write") {
        const auto* content = string_field(args, "content");
        if (!content) return failure("Invalid content");
        auto result = env.write(*path, *content, stop);
        if (!result) return std::unexpected(result.error());
        return text_result("Successfully wrote to " + *path);
    }
    if (name == "read") return read_path(*path, env, options, stop);
    auto content = env.read(*path, stop);
    if (!content) return std::unexpected(content.error());
    auto* edits = field(args, "edits");
    if (!edits || !edits->holds<support::JsonValue::array_t>() || edits->get_array().empty())
        return failure("Edit tool input is invalid. edits must contain at least one replacement.");
    struct Replacement {
        std::size_t offset;
        std::size_t length;
        std::string text;
    };
    std::vector<Replacement> replacements;
    for (const auto& edit : edits->get_array()) {
        const auto* old = string_field(edit, "oldText");
        const auto* replacement = string_field(edit, "newText");
        if (!old || !replacement || old->empty()) return failure("Invalid edit");
        auto position = content->find(*old);
        if (position == std::string::npos || content->find(*old, position + 1) != std::string::npos)
            return failure("Edit text must match exactly once");
        replacements.push_back({position, old->size(), *replacement});
    }
    std::ranges::sort(replacements, {}, &Replacement::offset);
    for (std::size_t i = 1; i < replacements.size(); ++i)
        if (replacements[i - 1].offset + replacements[i - 1].length > replacements[i].offset)
            return failure("Edits overlap");
    for (auto it = replacements.rbegin(); it != replacements.rend(); ++it)
        content->replace(it->offset, it->length, it->text);
    auto written = env.write(*path, *content, stop);
    if (!written) return std::unexpected(written.error());
    return text_result(std::format("Successfully replaced {} block(s) in {}.", replacements.size(), *path));
}
support::JsonValue parameters(std::string_view name) {
    using Json = support::JsonValue;
    auto string_schema = [] { return Json::object_t{{"type", "string"}}; };
    Json::object_t properties;
    Json::array_t required;
    auto add_string = [&](std::string key) {
        properties[key] = string_schema();
        required.emplace_back(key);
    };
    if (name == "bash") {
        add_string("command");
        properties["timeout"] = Json::object_t{{"type", "number"}};
    } else {
        add_string("path");
        if (name == "write") add_string("content");
        if (name == "edit") {
            Json::object_t edit_properties{{"oldText", string_schema()}, {"newText", string_schema()}};
            Json::object_t item{{"type", "object"},
                    {"properties", std::move(edit_properties)},
                    {"required", Json::array_t{"oldText", "newText"}}};
            properties["edits"] = Json::object_t{{"type", "array"}, {"items", std::move(item)}};
            required.emplace_back("edits");
        }
    }
    return Json::object_t{{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}};
}
} // namespace

Extension coding_tools(std::shared_ptr<Environment> environment, CodingToolOptions options) {
    Extension result{.name = "coding-tools"};
    // The four tool lambdas are copied into the Registry and share one
    // immutable options object for as long as their registrations live.
    auto shared_options = std::make_shared<const CodingToolOptions>(std::move(options));
    for (std::string name : {"read", "write", "edit", "bash"}) {
        result.tools.push_back(ToolRegistration{.declaration = ai::Tool{.name = name,
                                                        .description = "Durable " + name + " tool",
                                                        .parameters = parameters(name)},
                .execute = [environment, name, shared_options](support::JsonValue args, std::stop_token stop) {
                    if (!environment)
                        return support::AsyncResult<std::vector<ai::Content>>{failure("No execution environment")};
                    return support::AsyncResult<std::vector<ai::Content>>{
                            run(name, args, *environment, *shared_options, stop)};
                }});
    }
    return result;
}
} // namespace cch::durable
