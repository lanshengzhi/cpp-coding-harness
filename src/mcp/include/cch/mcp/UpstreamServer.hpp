#pragma once

#include <cch/mcp/UpstreamTool.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::mcp {

/// What a `server/discover` probe learned about one Upstream MCP Server.
///
/// The era probe fixes the protocol revision for the whole connection, so a
/// probe result whose revision this build does not speak, whose required
/// fields are missing, or whose declared capability set this build does not
/// implement is rejected outright rather than partially believed.
///
/// `name` is the server's self-reported display name and never an identity:
/// the Server Id from configuration is the sole stable identity
/// (`CONTEXT.md`).
struct UpstreamServerInfo {
    std::string name{};
    std::string version{};
    /// The server capabilities this build understands, sorted by name. An
    /// unrecognized declared capability fails the probe.
    std::vector<std::string> capabilities{};
    /// Server-provided usage guidance surfaced to the model (spec #833
    /// story 18). Absent when the server offers none.
    std::string instructions{};
};

/// The scope an Upstream that declares no `cacheScope` of its own belongs to.
/// `session` is the narrowest one the wire vocabulary has: a catalog is
/// reusable only by a cache that reads the same scope, so an Upstream that
/// says nothing about reuse is never reused more widely than the session
/// that fetched it (spec #833 story 17).
inline constexpr std::string_view kDefaultCatalogCacheScope{"session"};

/// The widest scope the wire vocabulary has: an Upstream that declares
/// `cacheScope: "process"` has said its catalog may be reused by every session
/// one host process runs. Only a host-owned cache reads this scope, and the
/// lifetime that accepts is ADR 0067's decision.
inline constexpr std::string_view kProcessCatalogCacheScope{"process"};

/// One Upstream MCP Server's tool catalog as the MCP Host holds it, in the
/// server's own `tools/list` order (servers SHOULD return a deterministic
/// order, and the catalog cache preserves it).
struct UpstreamCatalog {
    std::vector<UpstreamToolDescriptor> tools{};
    std::string instructions{};
    /// How long the Upstream said this catalog stays fresh, as its `ttlMs`
    /// hint. Absent means the Upstream offered no hint, and the catalog is
    /// therefore not reusable at all: a cache is a shortcut the Upstream has
    /// to ask for, never a default.
    std::optional<std::chrono::milliseconds> freshness{std::nullopt};
    /// The scope the Upstream said this catalog may be reused in, as its
    /// `cacheScope`. An Upstream that declares none is in the narrowest
    /// scope, never a wider one.
    std::string cache_scope{kDefaultCatalogCacheScope};
};

} // namespace cch::mcp
