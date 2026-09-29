#include <cch/mcp/UpstreamCatalogCache.hpp>

#include "mcp/Protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace cch::mcp {
namespace {

using support::ErrorCode;
using support::make_error;

/// The value-shaped rules a cold `tools/list` walk enforces and a cached
/// catalog therefore has to be re-checked against. The walk-shaped rules (the
/// pagination-cursor cap, duplicate and repeated cursors) are not value
/// properties and cannot be captured in an entry; they hold because a
/// refresh is a real walk.
[[nodiscard]] std::optional<support::Error> reject_unadmissible(const UpstreamCatalog& catalog) {
    if (catalog.tools.size() > protocol::kMaxToolsPerUpstream) {
        return make_error(ErrorCode::ResourceLimit,
                "the MCP Host will not cache a tool catalog it would not admit",
                "the " + std::to_string(protocol::kMaxToolsPerUpstream) + "-tool per-server cap is exceeded");
    }
    std::set<std::string> names;
    for (const auto& tool : catalog.tools) {
        if (!names.insert(tool.name).second) {
            return make_error(ErrorCode::Validation,
                    "the MCP Host will not cache a tool catalog with a duplicate tool name",
                    "the untrusted name is not echoed");
        }
    }
    return std::nullopt;
}

} // namespace

UpstreamCatalogCache::UpstreamCatalogCache(UpstreamCatalogCacheOptions options) : options(std::move(options)) {}

const std::string& UpstreamCatalogCache::scope() const noexcept { return options.scope; }

std::optional<CachedUpstreamCatalog> UpstreamCatalogCache::lookup(
        std::string_view server_id, std::string_view url) const {
    const std::scoped_lock lock(mutex);
    const auto found = std::ranges::find_if(
            entries, [server_id, url](const Entry& entry) { return entry.server_id == server_id && entry.url == url; });
    if (found == entries.end()) {
        return std::nullopt;
    }
    return CachedUpstreamCatalog{
            .catalog = found->catalog,
            .stale = std::chrono::steady_clock::now() >= found->expires_at,
    };
}

support::ExpectedVoid UpstreamCatalogCache::admit(
        std::string_view server_id, std::string_view url, const UpstreamCatalog& catalog) {
    if (!catalog.freshness.has_value() || catalog.cache_scope != options.scope) {
        return support::ExpectedVoid{};
    }
    if (const auto rejected = reject_unadmissible(catalog); rejected.has_value()) {
        return std::unexpected(std::move(*rejected));
    }
    const std::scoped_lock lock(mutex);
    std::erase_if(entries, [server_id](const Entry& entry) { return entry.server_id == server_id; });
    entries.push_back(Entry{
            .server_id = std::string(server_id),
            .url = std::string(url),
            .catalog = catalog,
            .expires_at = std::chrono::steady_clock::now() + *catalog.freshness,
    });
    // The cache is the one resource the MCP Host holds across sessions, so it
    // is bounded in entries: at the bound the oldest entry is dropped rather
    // than the newest one refused, because a refused entry would silently
    // disable caching for a server the user still has.
    while (entries.size() > protocol::kMaxCachedCatalogs) {
        entries.erase(entries.begin());
    }
    return support::ExpectedVoid{};
}

std::size_t UpstreamCatalogCache::size() const noexcept {
    const std::scoped_lock lock(mutex);
    return entries.size();
}

void UpstreamCatalogCache::clear() noexcept {
    const std::scoped_lock lock(mutex);
    entries.clear();
}

} // namespace cch::mcp
