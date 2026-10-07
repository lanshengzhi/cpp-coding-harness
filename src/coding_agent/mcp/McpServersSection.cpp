#include "coding_agent/mcp/McpServersSection.hpp"

#include "coding_agent/mcp/McpNamespace.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::coding_agent::mcp {
namespace {

/// pi `String.prototype.trim()`.
[[nodiscard]] std::string trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string{text.substr(begin, end - begin)};
}

/// The `McpServerConfigBase` half of an entry's descriptor.
[[nodiscard]] const McpServerConfigBase& config_base(const McpConfigEntry& entry) {
    return std::visit([](const auto& config) -> const McpServerConfigBase& { return config; }, entry.config);
}

[[nodiscard]] bool has_exposure(std::span<const McpExposure> exposures, McpExposure needle) {
    return std::find(exposures.begin(), exposures.end(), needle) != exposures.end();
}

[[nodiscard]] bool has_indirect_tools(std::span<const McpExposure> exposures) {
    return has_exposure(exposures, McpExposure::Codemode) || has_exposure(exposures, McpExposure::Deferred);
}

/// pi `truncate(text, max)`: an over-long text becomes its `max - 1` leading
/// code units with trailing whitespace trimmed, then an ellipsis.
[[nodiscard]] std::string truncate(std::string text, std::size_t max) {
    if (text.size() <= max) {
        return text;
    }
    if (max <= 1) {
        return {};
    }
    text.resize(max - 1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    text += "\xE2\x80\xA6"; // U+2026 HORIZONTAL ELLIPSIS
    return text;
}

/// pi `serverSummary`: the configured description's first line, else the
/// connection's `instructions` first line, trimmed.
[[nodiscard]] std::string server_summary(const McpServerListing& server) {
    std::string text;
    if (server.entry != nullptr) {
        if (const auto& description = config_base(*server.entry).description; description.has_value()) {
            text = trim(*description);
        }
    }
    if (text.empty()) {
        text = server.instructions.value_or(std::string{});
    }
    if (const auto newline = text.find('\n'); newline != std::string::npos) {
        text.resize(newline);
    }
    return trim(text);
}

/// pi `serversSectionIntro`: the ways of reaching tools the listed servers use.
[[nodiscard]] std::string servers_section_intro(bool reaches_codemode, bool reaches_tool_search) {
    std::string intro = "MCP servers whose tools are not declared to you.";
    if (reaches_codemode) {
        intro += " Call the tools of `codemode` servers from codemode scripts.";
    }
    if (reaches_tool_search) {
        intro += " Load the tools of `tool_search` servers with `tool_search`.";
    }
    return intro;
}

/// pi `omitted(count)`: the closing line counting servers that did not fit.
[[nodiscard]] std::string omitted_line(std::size_t count) {
    if (count == 0) {
        return {};
    }
    return "- \xE2\x80\xA6 " + std::to_string(count) + " more server" + (count == 1 ? "" : "s") +
           "; find their tools with searchTools()";
}

} // namespace

std::vector<McpExposure> mcp_configured_exposures(const McpConfigEntry& entry) {
    const McpServerConfigBase& base = config_base(entry);
    std::vector<McpExposure> exposures;
    exposures.reserve(1 + base.tool_exposure.size());
    exposures.push_back(base.exposure.value_or(McpExposure::Codemode));
    for (const auto& [_, exposure] : base.tool_exposure) {
        exposures.push_back(exposure);
    }
    std::sort(exposures.begin(), exposures.end(), [](McpExposure left, McpExposure right) {
        return static_cast<int>(left) < static_cast<int>(right);
    });
    exposures.erase(std::unique(exposures.begin(), exposures.end()), exposures.end());
    return exposures;
}

std::optional<std::string> render_mcp_servers_section(std::span<const McpServerListing> servers) {
    std::vector<McpServerListing> listed;
    for (const auto& server : servers) {
        if (server.entry == nullptr || !server.entry->enabled) {
            continue;
        }
        if (!has_indirect_tools(mcp_configured_exposures(*server.entry))) {
            continue;
        }
        listed.push_back(server);
    }
    if (listed.empty()) {
        return std::nullopt;
    }
    std::sort(listed.begin(), listed.end(), [](const McpServerListing& left, const McpServerListing& right) {
        return left.entry->name < right.entry->name;
    });

    bool reaches_codemode = false;
    bool reaches_tool_search = false;
    std::vector<std::string> reaches;
    reaches.reserve(listed.size());
    for (const auto& server : listed) {
        if (has_exposure(mcp_configured_exposures(*server.entry), McpExposure::Codemode)) {
            reaches_codemode = true;
            reaches.push_back("codemode");
        } else {
            reaches_tool_search = true;
            reaches.push_back("tool_search");
        }
    }
    const std::string intro = servers_section_intro(reaches_codemode, reaches_tool_search);

    std::vector<std::string> heads;
    heads.reserve(listed.size());
    for (std::size_t index = 0; index < listed.size(); ++index) {
        heads.push_back("- " + detail::mcp_namespace(listed[index].entry->name) + " (" + reaches[index] + ")");
    }

    // Characters of the intro, the first `kept` server lines without
    // descriptions, and the omission line.
    const auto section_size = [&](std::size_t kept) -> std::size_t {
        std::string text = intro;
        for (std::size_t index = 0; index < kept; ++index) {
            text += "\n";
            text += heads[index];
        }
        if (const std::string omitted = omitted_line(listed.size() - kept); !omitted.empty()) {
            text += "\n";
            text += omitted;
        }
        return text.size();
    };

    std::size_t kept = listed.size();
    while (kept > 0 && section_size(kept) > kMcpMaxServersSectionChars) {
        --kept;
    }

    // Each description also takes a ": " separator.
    long long per_server = 0;
    if (kept > 0) {
        const auto budget =
                static_cast<long long>(kMcpMaxServersSectionChars) - static_cast<long long>(section_size(kept));
        per_server = budget / static_cast<long long>(kept) - 2;
        per_server = std::max<long long>(0, per_server);
        per_server = std::min<long long>(static_cast<long long>(kMcpMaxServerDescriptionChars), per_server);
    }

    std::string text = intro;
    for (std::size_t index = 0; index < kept; ++index) {
        text += "\n";
        text += heads[index];
        if (per_server > 0) {
            const std::string summary = truncate(server_summary(listed[index]), static_cast<std::size_t>(per_server));
            if (!summary.empty()) {
                text += ": ";
                text += summary;
            }
        }
    }
    if (const std::string omitted = omitted_line(listed.size() - kept); !omitted.empty()) {
        text += "\n";
        text += omitted;
    }
    return text;
}

} // namespace cch::coding_agent::mcp
