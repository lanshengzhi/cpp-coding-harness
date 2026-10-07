// The write half of pi's `mcp.json` persistence (spec #882, ticket #884). pi
// source at `7c10bd43` (v1.0.4): `packages/coding-agent/src/extensions/mcp/
// config.ts`. `editMcpServers` reads the file (an empty config when missing),
// lets the caller change `mcpServers`, and rewrites the whole document with the
// file's own indentation and a trailing newline, so unrelated content survives.
//
// Two pi behaviors the shared `support::JsonValue` cannot express force a
// private, ordered JSON model here: pi's `JSON.parse`/`JSON.stringify` preserve
// key insertion order, and `writeFileSync` is replaced by a temp-file rename so
// a reader never observes a torn file. `cch_support`'s `JsonValue::object_t` is
// a sorted `std::map`, so this file parses into and serializes from its own
// insertion-ordered node instead of changing that cross-Owner type. Values the
// caller supplies through `support::JsonValue` are converted into the ordered
// node at the point of insertion; their own key order is whatever that value
// carries.
//
// The file format is private to `cch_coding_agent`; nothing here is an Owner
// Interface.

#include "coding_agent/mcp/McpConfigWrite.hpp"

#include "support/Json.hpp"

#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::coding_agent::mcp {
namespace {

/// One insertion-ordered JSON document node, the private model of this write
/// half. Objects keep their members in parse/insertion order (`std::map` would
/// reorder them), which is what makes a read-modify-write byte-stable.
struct JsonNode {
    using array_t = std::vector<JsonNode>;
    using member_t = std::pair<std::string, JsonNode>;
    using object_t = std::vector<member_t>;
    using value_t = std::variant<std::nullptr_t, bool, double, std::string, array_t, object_t>;

    value_t data{nullptr};

    JsonNode() = default;
    explicit JsonNode(std::nullptr_t) : data(nullptr) {}
    explicit JsonNode(bool value) : data(value) {}
    explicit JsonNode(double value) : data(value) {}
    explicit JsonNode(std::string value) : data(std::move(value)) {}
    explicit JsonNode(array_t value) : data(std::move(value)) {}
    explicit JsonNode(object_t value) : data(std::move(value)) {}

    [[nodiscard]] object_t* as_object() noexcept { return std::get_if<object_t>(&data); }
    [[nodiscard]] const object_t* as_object() const noexcept { return std::get_if<object_t>(&data); }
    [[nodiscard]] bool is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(data); }

    [[nodiscard]] JsonNode* find(std::string_view key) noexcept {
        auto* object = as_object();
        if (object == nullptr) {
            return nullptr;
        }
        for (auto& [name, child] : *object) {
            if (name == key) {
                return &child;
            }
        }
        return nullptr;
    }

    /// Assign `value` at `key`, keeping the key's position when it exists and
    /// appending it otherwise (pi `object[key] = value`).
    void insert_or_assign(std::string key, JsonNode value) {
        auto* object = as_object();
        if (object == nullptr) {
            return;
        }
        for (auto& [name, child] : *object) {
            if (name == key) {
                child = std::move(value);
                return;
            }
        }
        object->emplace_back(std::move(key), std::move(value));
    }

    void erase(std::string_view key) {
        auto* object = as_object();
        if (object == nullptr) {
            return;
        }
        for (auto it = object->begin(); it != object->end(); ++it) {
            if (it->first == key) {
                object->erase(it);
                return;
            }
        }
    }

    [[nodiscard]] bool contains(std::string_view key) const noexcept {
        const auto* object = as_object();
        if (object == nullptr) {
            return false;
        }
        for (const auto& [name, child] : *object) {
            if (name == key) {
                return true;
            }
        }
        return false;
    }
};

[[nodiscard]] support::Error write_error(std::string message) {
    return support::make_error(support::ErrorCode::Validation, std::move(message));
}

// --- A small strict JSON parser producing the ordered node --------------------

class JsonReader final {
public:
    explicit JsonReader(std::string_view text) : text_(text) {}

    [[nodiscard]] bool parse_document(JsonNode& out) {
        skip_whitespace();
        if (!parse_value(out)) {
            return false;
        }
        // pi `JSON.parse` rejects trailing content; this write half keeps the
        // stricter reading so a malformed file is never half-honored.
        skip_whitespace();
        return position_ == text_.size();
    }

private:
    [[nodiscard]] bool parse_value(JsonNode& out) {
        skip_whitespace();
        if (position_ >= text_.size()) {
            return false;
        }
        switch (text_[position_]) {
        case '{':
            return parse_object(out);
        case '[':
            return parse_array(out);
        case '"': {
            std::string value;
            if (!parse_string(value)) {
                return false;
            }
            out = JsonNode{std::move(value)};
            return true;
        }
        case 't':
            return parse_literal("true", JsonNode{true}, out);
        case 'f':
            return parse_literal("false", JsonNode{false}, out);
        case 'n':
            return parse_literal("null", JsonNode{nullptr}, out);
        default:
            return parse_number(out);
        }
    }

    [[nodiscard]] bool parse_object(JsonNode& out) {
        ++position_; // '{'
        JsonNode::object_t members;
        skip_whitespace();
        if (position_ < text_.size() && text_[position_] == '}') {
            ++position_;
            out = JsonNode{std::move(members)};
            return true;
        }
        for (;;) {
            skip_whitespace();
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            skip_whitespace();
            if (position_ >= text_.size() || text_[position_] != ':') {
                return false;
            }
            ++position_;
            JsonNode value;
            if (!parse_value(value)) {
                return false;
            }
            members.emplace_back(std::move(key), std::move(value));
            skip_whitespace();
            if (position_ >= text_.size()) {
                return false;
            }
            if (text_[position_] == ',') {
                ++position_;
                continue;
            }
            if (text_[position_] == '}') {
                ++position_;
                out = JsonNode{std::move(members)};
                return true;
            }
            return false;
        }
    }

    [[nodiscard]] bool parse_array(JsonNode& out) {
        ++position_; // '['
        JsonNode::array_t items;
        skip_whitespace();
        if (position_ < text_.size() && text_[position_] == ']') {
            ++position_;
            out = JsonNode{std::move(items)};
            return true;
        }
        for (;;) {
            JsonNode value;
            if (!parse_value(value)) {
                return false;
            }
            items.push_back(std::move(value));
            skip_whitespace();
            if (position_ >= text_.size()) {
                return false;
            }
            if (text_[position_] == ',') {
                ++position_;
                continue;
            }
            if (text_[position_] == ']') {
                ++position_;
                out = JsonNode{std::move(items)};
                return true;
            }
            return false;
        }
    }

    [[nodiscard]] bool parse_string(std::string& out) {
        if (position_ >= text_.size() || text_[position_] != '"') {
            return false;
        }
        ++position_;
        out.clear();
        while (position_ < text_.size()) {
            const char character = text_[position_++];
            if (character == '"') {
                return true;
            }
            if (character != '\\') {
                out.push_back(character);
                continue;
            }
            if (position_ >= text_.size()) {
                return false;
            }
            const char escape = text_[position_++];
            switch (escape) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                if (!decode_unicode(out)) {
                    return false;
                }
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    [[nodiscard]] bool decode_unicode(std::string& out) {
        const auto code_unit = [this](unsigned int& unit) {
            if (position_ + 4 > text_.size()) {
                return false;
            }
            unit = 0;
            for (int index = 0; index < 4; ++index) {
                const char character = text_[position_++];
                unit <<= 4U;
                if (character >= '0' && character <= '9') {
                    unit |= static_cast<unsigned int>(character - '0');
                } else if (character >= 'a' && character <= 'f') {
                    unit |= static_cast<unsigned int>(character - 'a' + 10);
                } else if (character >= 'A' && character <= 'F') {
                    unit |= static_cast<unsigned int>(character - 'A' + 10);
                } else {
                    return false;
                }
            }
            return true;
        };
        unsigned int first = 0;
        if (!code_unit(first)) {
            return false;
        }
        unsigned int code_point = first;
        if (first >= 0xD800 && first <= 0xDBFF) {
            if (position_ + 2 > text_.size() || text_[position_] != '\\' || text_[position_ + 1] != 'u') {
                return false;
            }
            position_ += 2;
            unsigned int second = 0;
            if (!code_unit(second) || second < 0xDC00 || second > 0xDFFF) {
                return false;
            }
            code_point = 0x10000U + ((first - 0xD800U) << 10U) + (second - 0xDC00U);
        } else if (first >= 0xDC00 && first <= 0xDFFF) {
            return false;
        }
        append_utf8(code_point, out);
        return true;
    }

    static void append_utf8(unsigned int code_point, std::string& out) {
        if (code_point < 0x80U) {
            out.push_back(static_cast<char>(code_point));
        } else if (code_point < 0x800U) {
            out.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
            out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else if (code_point < 0x10000U) {
            out.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        }
    }

    [[nodiscard]] bool parse_literal(std::string_view literal, JsonNode value, JsonNode& out) {
        if (text_.substr(position_, literal.size()) != literal) {
            return false;
        }
        position_ += literal.size();
        out = std::move(value);
        return true;
    }

    [[nodiscard]] bool parse_number(JsonNode& out) {
        const std::size_t start = position_;
        if (position_ < text_.size() && text_[position_] == '-') {
            ++position_;
        }
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
            ++position_;
        }
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) {
                ++position_;
            }
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ == start) {
            return false;
        }
        const std::string literal{text_.substr(start, position_ - start)};
        char* end = nullptr;
        const double value = std::strtod(literal.c_str(), &end);
        if (end != literal.c_str() + literal.size()) {
            return false;
        }
        out = JsonNode{value};
        return true;
    }

    void skip_whitespace() {
        while (position_ < text_.size()) {
            const char character = text_[position_];
            if (character != ' ' && character != '\t' && character != '\n' && character != '\r') {
                break;
            }
            ++position_;
        }
    }

    std::string_view text_;
    std::size_t position_{0};
};

/// The file's text, or `std::nullopt` when it does not exist or cannot be read.
[[nodiscard]] std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error) || exists_error) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return std::nullopt;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

/// pi `editMcpServers`'s indentation detection: the first line's leading run of
/// spaces or tabs before a non-whitespace character (`/^([ \t]+)\S/m`), else
/// two spaces.
[[nodiscard]] std::string detect_indent(std::string_view text) {
    std::size_t line_start = 0;
    while (line_start < text.size()) {
        std::size_t run = 0;
        while (line_start + run < text.size() && (text[line_start + run] == ' ' || text[line_start + run] == '\t')) {
            ++run;
        }
        if (run > 0 && line_start + run < text.size() && text[line_start + run] != '\n' &&
                text[line_start + run] != '\r') {
            return std::string{text.substr(line_start, run)};
        }
        const auto newline = text.find('\n', line_start);
        if (newline == std::string_view::npos) {
            break;
        }
        line_start = newline + 1;
    }
    return "  ";
}

/// `JSON.stringify(value, null, indent)`: object and array entries on their own
/// lines, `indent` repeated per depth, empty containers as `{}`/`[]`. Object
/// members keep the node's insertion order.
[[nodiscard]] support::ExpectedVoid append_indented(
        const JsonNode& value, std::string& out, std::string_view indent, std::size_t depth) {
    const auto pad = [&](std::size_t levels) {
        for (std::size_t level = 0; level < levels; ++level) {
            out.append(indent);
        }
    };
    if (const auto* object = value.as_object()) {
        if (object->empty()) {
            out += "{}";
            return {};
        }
        out += "{\n";
        std::size_t index = 0;
        for (const auto& [key, child] : *object) {
            pad(depth + 1);
            auto key_json = support::write_json(support::JsonValue{key});
            if (!key_json) {
                return std::unexpected(key_json.error());
            }
            out.append(*key_json);
            out += ": ";
            if (auto appended = append_indented(child, out, indent, depth + 1); !appended) {
                return appended;
            }
            if (++index < object->size()) {
                out += ",";
            }
            out += "\n";
        }
        pad(depth);
        out += "}";
        return {};
    }
    if (const auto* array = std::get_if<JsonNode::array_t>(&value.data)) {
        if (array->empty()) {
            out += "[]";
            return {};
        }
        out += "[\n";
        for (std::size_t index = 0; index < array->size(); ++index) {
            pad(depth + 1);
            if (auto appended = append_indented((*array)[index], out, indent, depth + 1); !appended) {
                return appended;
            }
            if (index + 1 < array->size()) {
                out += ",";
            }
            out += "\n";
        }
        pad(depth);
        out += "]";
        return {};
    }
    auto scalar = support::write_json([&] {
        if (const auto* boolean = std::get_if<bool>(&value.data)) {
            return support::JsonValue{*boolean};
        }
        if (const auto* number = std::get_if<double>(&value.data)) {
            return support::JsonValue{*number};
        }
        if (const auto* text = std::get_if<std::string>(&value.data)) {
            return support::JsonValue{*text};
        }
        return support::JsonValue{nullptr};
    }());
    if (!scalar) {
        return std::unexpected(scalar.error());
    }
    out.append(*scalar);
    return {};
}

/// Write `text` through a temporary file and a rename in the same directory, so
/// a concurrent reader never observes a torn `mcp.json`.
[[nodiscard]] support::ExpectedVoid write_file(const std::filesystem::path& path, const std::string& text) {
    std::error_code directory_error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), directory_error);
        if (directory_error) {
            return std::unexpected(write_error("could not create mcp.json directory: " + directory_error.message()));
        }
    }
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return std::unexpected(write_error("could not open " + temporary.string() + " for writing"));
        }
        output << text;
        if (!output) {
            return std::unexpected(write_error("could not write " + temporary.string()));
        }
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, path, rename_error);
    if (rename_error) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        return std::unexpected(write_error("could not replace " + path.string() + ": " + rename_error.message()));
    }
    return {};
}

/// Convert a caller-supplied `support::JsonValue` into the ordered node. The
/// value's own object key order is whatever that value carries.
[[nodiscard]] JsonNode to_node(const support::JsonValue& value) {
    if (value.get_if<std::nullptr_t>() != nullptr) {
        return JsonNode{nullptr};
    }
    if (const auto* boolean = value.get_if<bool>()) {
        return JsonNode{*boolean};
    }
    if (const auto* number = value.get_if<double>()) {
        return JsonNode{*number};
    }
    if (const auto* text = value.get_if<std::string>()) {
        return JsonNode{*text};
    }
    if (const auto* array = value.get_if<support::JsonValue::array_t>()) {
        JsonNode::array_t items;
        items.reserve(array->size());
        for (const auto& item : *array) {
            items.push_back(to_node(item));
        }
        return JsonNode{std::move(items)};
    }
    if (const auto* object = value.get_if<support::JsonValue::object_t>()) {
        JsonNode::object_t members;
        members.reserve(object->size());
        for (const auto& [key, child] : *object) {
            members.emplace_back(key, to_node(child));
        }
        return JsonNode{std::move(members)};
    }
    return JsonNode{nullptr};
}

/// pi `editMcpServers`: read `path` (empty config when missing), hand the root
/// object to `edit`, and rewrite the file with its indentation when `edit`
/// returns true. `edit` returning an error aborts without touching the file.
[[nodiscard]] support::ExpectedVoid edit_mcp_servers(
        const std::filesystem::path& path, const std::function<support::Expected<bool>(JsonNode::object_t&)>& edit) {
    const auto text = read_text_file(path);
    JsonNode parsed{JsonNode::object_t{}};
    if (text) {
        JsonReader reader{*text};
        if (!reader.parse_document(parsed) || parsed.as_object() == nullptr) {
            return std::unexpected(write_error(path.string() + ": contains invalid JSON"));
        }
    }
    auto* root = parsed.as_object();
    if (root == nullptr) {
        return std::unexpected(write_error(path.string() + ": expected an object with an \"mcpServers\" object"));
    }
    if (const auto* servers = parsed.find("mcpServers"); servers != nullptr && servers->as_object() == nullptr) {
        return std::unexpected(write_error(path.string() + ": expected an object with an \"mcpServers\" object"));
    }
    auto changed = edit(*root);
    if (!changed) {
        return std::unexpected(std::move(changed.error()));
    }
    if (!*changed) {
        return {};
    }
    const std::string indent = text ? detect_indent(*text) : "  ";
    std::string out;
    if (auto appended = append_indented(parsed, out, indent, 0); !appended) {
        return std::unexpected(std::move(appended.error()));
    }
    out += "\n";
    return write_file(path, out);
}

/// The `mcpServers` object of `root`, creating it when `create` is set.
[[nodiscard]] JsonNode* servers_node(JsonNode::object_t& root, bool create) {
    for (auto& [key, child] : root) {
        if (key == "mcpServers") {
            return child.as_object() != nullptr ? &child : nullptr;
        }
    }
    if (!create) {
        return nullptr;
    }
    root.emplace_back("mcpServers", JsonNode{JsonNode::object_t{}});
    return &root.back().second;
}

/// pi `isOverride`: an entry with no `command`, `url`, or `type`.
[[nodiscard]] bool is_override_entry(const JsonNode& entry) {
    return !entry.contains("command") && !entry.contains("url") && !entry.contains("type");
}

} // namespace

support::ExpectedVoid update_mcp_server_config(
        const std::filesystem::path& path, std::string_view name, const McpServerConfigPatch& patch, bool override) {
    const std::string server{name};
    return edit_mcp_servers(path, [&](JsonNode::object_t& root) -> support::Expected<bool> {
        JsonNode* servers = servers_node(root, override);
        JsonNode* entry = nullptr;
        if (servers != nullptr) {
            entry = servers->find(server);
            if (entry != nullptr && entry->as_object() == nullptr) {
                return std::unexpected(write_error(path.string() + " server \"" + server + "\" is not an object"));
            }
        }
        if (entry == nullptr) {
            if (!override) {
                return std::unexpected(write_error(path.string() + " does not define MCP server \"" + server + "\""));
            }
            if (servers == nullptr) {
                servers = servers_node(root, /* create */ true);
            }
            servers->insert_or_assign(server, JsonNode{JsonNode::object_t{}});
            entry = servers->find(server);
        }
        const bool keep_defaults = is_override_entry(*entry);
        if (patch.enabled) {
            if (*patch.enabled && !keep_defaults) {
                entry->erase("enabled");
            } else {
                entry->insert_or_assign("enabled", JsonNode{*patch.enabled});
            }
        }
        if (patch.exposure) {
            const std::string spelling{mcp_exposure_name(*patch.exposure)};
            if (*patch.exposure == McpExposure::Codemode && !keep_defaults) {
                entry->erase("exposure");
            } else {
                entry->insert_or_assign("exposure", JsonNode{spelling});
            }
        }
        return true;
    });
}

support::Expected<bool> add_mcp_server_config(
        const std::filesystem::path& path, std::string_view name, const support::JsonValue& config) {
    const std::string server{name};
    bool replaced = false;
    auto edited = edit_mcp_servers(path, [&](JsonNode::object_t& root) -> support::Expected<bool> {
        JsonNode* servers = servers_node(root, /* create */ true);
        replaced = servers->contains(server);
        servers->insert_or_assign(server, to_node(config));
        return true;
    });
    if (!edited) {
        return std::unexpected(std::move(edited.error()));
    }
    return replaced;
}

support::Expected<bool> remove_mcp_server_config(const std::filesystem::path& path, std::string_view name) {
    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error) || exists_error) {
        return false;
    }
    const std::string server{name};
    bool removed = false;
    auto edited = edit_mcp_servers(path, [&](JsonNode::object_t& root) -> support::Expected<bool> {
        JsonNode* servers = servers_node(root, /* create */ false);
        if (servers == nullptr) {
            return false;
        }
        removed = servers->contains(server);
        if (removed) {
            servers->erase(server);
        }
        return removed;
    });
    if (!edited) {
        return std::unexpected(std::move(edited.error()));
    }
    return removed;
}

} // namespace cch::coding_agent::mcp
