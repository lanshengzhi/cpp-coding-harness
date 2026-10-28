#include <cch/durable/Environment.hpp>
#include <cch/ai/Content.hpp>

#include <algorithm>
#include <chrono>
#include <format>

namespace cch::durable {
namespace {
using Result = support::Expected<std::vector<ai::Content>>;

Result error(std::string text) {
    return std::unexpected(support::make_error(support::ErrorCode::Tool, std::move(text)));
}
Result text_result(std::string text) { return std::vector<ai::Content>{ai::text_content(std::move(text))}; }
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
Result run(std::string_view name, const support::JsonValue& args, Environment& env, std::stop_token stop) {
    if (name == "bash") {
        const auto* command = string_field(args, "command");
        if (!command) return error("Invalid command");
        std::chrono::milliseconds timeout{0};
        if (auto* value = field(args, "timeout"); value && value->holds<double>() && value->get_number() > 0)
            timeout = std::chrono::milliseconds{static_cast<std::int64_t>(value->get_number() * 1000)};
        auto result = env.exec(*command, timeout, {}, stop);
        if (!result) return std::unexpected(result.error());
        if (result->exit_code != 0)
            return error(result->output + std::format("\nCommand exited with code {}", result->exit_code));
        return text_result(std::move(result->output));
    }
    const auto* path = string_field(args, "path");
    if (!path) return error("Invalid path");
    if (name == "write") {
        const auto* content = string_field(args, "content");
        if (!content) return error("Invalid content");
        auto result = env.write(*path, *content, stop);
        if (!result) return std::unexpected(result.error());
        return text_result("Successfully wrote to " + *path);
    }
    auto content = env.read(*path, stop);
    if (!content) return std::unexpected(content.error());
    if (name == "read") return text_result(std::move(*content));
    auto* edits = field(args, "edits");
    if (!edits || !edits->holds<support::JsonValue::array_t>() || edits->get_array().empty())
        return error("Edit tool input is invalid. edits must contain at least one replacement.");
    struct Replacement {
        std::size_t offset;
        std::size_t length;
        std::string text;
    };
    std::vector<Replacement> replacements;
    for (const auto& edit : edits->get_array()) {
        const auto* old = string_field(edit, "oldText");
        const auto* replacement = string_field(edit, "newText");
        if (!old || !replacement || old->empty()) return error("Invalid edit");
        auto position = content->find(*old);
        if (position == std::string::npos || content->find(*old, position + 1) != std::string::npos)
            return error("Edit text must match exactly once");
        replacements.push_back({position, old->size(), *replacement});
    }
    std::ranges::sort(replacements, {}, &Replacement::offset);
    for (std::size_t i = 1; i < replacements.size(); ++i)
        if (replacements[i - 1].offset + replacements[i - 1].length > replacements[i].offset)
            return error("Edits overlap");
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

Extension coding_tools(std::shared_ptr<Environment> environment) {
    Extension result{.name = "coding-tools"};
    for (std::string name : {"read", "write", "edit", "bash"}) {
        result.tools.push_back(ToolRegistration{.declaration = ai::Tool{.name = name,
                                                        .description = "Durable " + name + " tool",
                                                        .parameters = parameters(name)},
                .execute = [environment, name](support::JsonValue args, std::stop_token stop) {
                    if (!environment)
                        return support::AsyncResult<std::vector<ai::Content>>{error("No execution environment")};
                    return support::AsyncResult<std::vector<ai::Content>>{run(name, args, *environment, stop)};
                }});
    }
    return result;
}
} // namespace cch::durable
