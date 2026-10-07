// The write half of pi's `mcp.json` persistence (spec #882, ticket #884). pi
// source at `7c10bd43` (v1.0.4): `packages/coding-agent/src/extensions/mcp/
// config.ts`. `editMcpServers` reads the file (an empty config when missing),
// lets the caller change `mcpServers`, and rewrites the whole document with the
// file's own indentation and a trailing newline, so unrelated content survives.
// The file format is private to `cch_coding_agent`; nothing here is an Owner
// Interface.

#include "coding_agent/mcp/McpConfigWrite.hpp"

#include "support/Json.hpp"

#include <cstddef>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <utility>

namespace cch::coding_agent::mcp {
namespace {

using JsonObject = support::JsonValue::object_t;
using JsonArray = support::JsonValue::array_t;

[[nodiscard]] support::Error write_error(std::string message) {
    return support::make_error(support::ErrorCode::Validation, std::move(message));
}

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
/// lines, `indent` repeated per depth, empty containers as `{}`/`[]`.
[[nodiscard]] support::ExpectedVoid append_indented(
        const support::JsonValue& value, std::string& out, std::string_view indent, std::size_t depth) {
    const auto pad = [&](std::size_t levels) {
        for (std::size_t level = 0; level < levels; ++level) {
            out.append(indent);
        }
    };
    if (const auto* object = value.get_if<JsonObject>()) {
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
    if (const auto* array = value.get_if<JsonArray>()) {
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
    auto scalar = support::write_json(value);
    if (!scalar) {
        return std::unexpected(scalar.error());
    }
    out.append(*scalar);
    return {};
}

[[nodiscard]] support::ExpectedVoid write_file(const std::filesystem::path& path, const std::string& text) {
    std::error_code directory_error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), directory_error);
        if (directory_error) {
            return std::unexpected(write_error("could not create mcp.json directory: " + directory_error.message()));
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return std::unexpected(write_error("could not open mcp.json for writing"));
    }
    output << text;
    if (!output) {
        return std::unexpected(write_error("could not write mcp.json"));
    }
    return {};
}

/// pi `editMcpServers`: read `path` (empty config when missing), hand the root
/// object to `edit`, and rewrite the file with its indentation when `edit`
/// returns true. `edit` returning an error aborts without touching the file.
[[nodiscard]] support::ExpectedVoid edit_mcp_servers(
        const std::filesystem::path& path, const std::function<support::Expected<bool>(JsonObject&)>& edit) {
    const auto text = read_text_file(path);
    support::JsonValue parsed{JsonObject{}};
    if (text) {
        auto read = support::read_json(*text);
        if (!read) {
            return std::unexpected(write_error(path.string() + ": contains invalid JSON"));
        }
        parsed = std::move(*read);
    }
    auto* root = parsed.get_if<JsonObject>();
    if (root == nullptr) {
        return std::unexpected(write_error(path.string() + ": expected an object with an \"mcpServers\" object"));
    }
    if (const auto servers = root->find("mcpServers");
            servers != root->end() && servers->second.get_if<JsonObject>() == nullptr) {
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
[[nodiscard]] JsonObject* servers_object(JsonObject& root, bool create) {
    const auto found = root.find("mcpServers");
    if (found != root.end()) {
        return found->second.get_if<JsonObject>();
    }
    if (!create) {
        return nullptr;
    }
    root.emplace("mcpServers", JsonObject{});
    return root.find("mcpServers")->second.get_if<JsonObject>();
}

/// pi `isOverride`: an entry with no `command`, `url`, or `type`.
[[nodiscard]] bool is_override_entry(const JsonObject& entry) {
    return !entry.contains("command") && !entry.contains("url") && !entry.contains("type");
}

} // namespace

support::ExpectedVoid update_mcp_server_config(
        const std::filesystem::path& path, std::string_view name, const McpServerConfigPatch& patch, bool override) {
    const std::string server{name};
    return edit_mcp_servers(path, [&](JsonObject& root) -> support::Expected<bool> {
        JsonObject* servers = servers_object(root, override);
        JsonObject* entry = nullptr;
        if (servers != nullptr) {
            const auto found = servers->find(server);
            if (found != servers->end()) {
                entry = found->second.get_if<JsonObject>();
                if (entry == nullptr) {
                    return std::unexpected(write_error(path.string() + " server \"" + server + "\" is not an object"));
                }
            }
        }
        if (entry == nullptr) {
            if (!override) {
                return std::unexpected(write_error(path.string() + " does not define MCP server \"" + server + "\""));
            }
            if (servers == nullptr) {
                servers = servers_object(root, /* create */ true);
            }
            servers->emplace(server, JsonObject{});
            entry = servers->find(server)->second.get_if<JsonObject>();
        }
        const bool keep_defaults = is_override_entry(*entry);
        if (patch.enabled) {
            if (*patch.enabled && !keep_defaults) {
                entry->erase("enabled");
            } else {
                entry->insert_or_assign("enabled", support::JsonValue{*patch.enabled});
            }
        }
        if (patch.exposure) {
            const std::string spelling{mcp_exposure_name(*patch.exposure)};
            if (*patch.exposure == McpExposure::Codemode && !keep_defaults) {
                entry->erase("exposure");
            } else {
                entry->insert_or_assign("exposure", support::JsonValue{spelling});
            }
        }
        return true;
    });
}

support::Expected<bool> add_mcp_server_config(
        const std::filesystem::path& path, std::string_view name, const support::JsonValue& config) {
    const std::string server{name};
    bool replaced = false;
    auto edited = edit_mcp_servers(path, [&](JsonObject& root) -> support::Expected<bool> {
        JsonObject* servers = servers_object(root, /* create */ true);
        replaced = servers->contains(server);
        servers->insert_or_assign(server, config);
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
    auto edited = edit_mcp_servers(path, [&](JsonObject& root) -> support::Expected<bool> {
        JsonObject* servers = servers_object(root, /* create */ false);
        if (servers == nullptr) {
            return false;
        }
        removed = servers->erase(server) > 0;
        return removed;
    });
    if (!edited) {
        return std::unexpected(std::move(edited.error()));
    }
    return removed;
}

} // namespace cch::coding_agent::mcp
