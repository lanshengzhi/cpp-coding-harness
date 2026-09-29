#pragma once

#include <cch/mcp/UpstreamServer.hpp>
#include <cch/support/Error.hpp>

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::mcp {

struct UpstreamCatalogCacheOptions {
    /// The reuse scope this cache reads and writes, as the `cacheScope` an
    /// Upstream declares. Two caches with different scopes share nothing: an
    /// entry stored under one is never served to a lookup under the other,
    /// and an Upstream that declares a scope this cache does not hold is not
    /// cached in it at all. The default is the narrowest scope, so a cache
    /// that is not told otherwise never serves a catalog more widely than
    /// the session that fetched it.
    std::string scope{kDefaultCatalogCacheScope};
};

/// One reading of a cached catalog. `stale` is the Upstream's own freshness
/// hint having run out: the catalog is still the best the MCP Host has, and
/// it is what a caller is served while the refresh behind it runs.
struct CachedUpstreamCatalog {
    UpstreamCatalog catalog{};
    bool stale{false};
};

/// The MCP Host's per-Upstream tool-catalog cache, honouring the
/// `ttlMs`/`cacheScope` hints an Upstream provides (spec #833 story 17;
/// issue #848).
///
/// It is a shortcut, never a source of failure: a cache that holds nothing
/// behaves exactly as the client stack behaved before it existed, and a
/// catalog this cache refuses to admit still reaches the caller from the
/// walk that produced it. The host shares one instance across connections
/// and across sessions, which is what makes a repeated session skip the
/// `tools/list` walk the first one already paid for.
///
/// Entries are bounded in count and in how long an Upstream's own hint can
/// keep one alive; the values and their selection live in
/// `docs/runtime-capacities.md`.
///
/// Not copyable and not movable: it is deliberately shared live state,
/// carried by `std::shared_ptr` (§7.4), and it may be read and written from
/// more than one session's execution domain, so every operation takes the
/// cache's own lock.
class UpstreamCatalogCache {
public:
    explicit UpstreamCatalogCache(UpstreamCatalogCacheOptions options = {});
    UpstreamCatalogCache(const UpstreamCatalogCache&) = delete;
    UpstreamCatalogCache& operator=(const UpstreamCatalogCache&) = delete;
    UpstreamCatalogCache(UpstreamCatalogCache&&) = delete;
    UpstreamCatalogCache& operator=(UpstreamCatalogCache&&) = delete;

    /// The scope this cache reads and writes.
    [[nodiscard]] const std::string& scope() const noexcept;

    /// The catalog cached for one Server Id under this cache's own scope, or
    /// `std::nullopt` when this cache holds nothing for it. A lookup never
    /// changes what the cache holds, so it is safe to read a cache another
    /// session's connection is filling.
    [[nodiscard]] std::optional<CachedUpstreamCatalog> lookup(std::string_view server_id) const;

    /// Offer one catalog to this cache.
    ///
    /// A catalog is untrusted input whatever produced it, so the value-shaped
    /// rules a cold `tools/list` walk enforces are re-run here rather than
    /// assumed: a catalog over the per-server tool cap, or one that names a
    /// tool twice, is refused with an error. The walk-shaped rules — the
    /// pagination-cursor cap and the cursor-loop rejection — are properties
    /// of the walk rather than of the value it produces, so they cannot be
    /// captured in an entry at all; they hold because a refresh is a real
    /// walk.
    ///
    /// Success means the cache is now correct, which includes the two cases
    /// where there is correctly nothing to store: an Upstream that offered no
    /// freshness hint asked for no reuse, and an Upstream that declared
    /// another scope has an entry that belongs to a different cache.
    [[nodiscard]] support::ExpectedVoid admit(std::string_view server_id, const UpstreamCatalog& catalog);

    /// How many entries this cache holds.
    [[nodiscard]] std::size_t size() const noexcept;

    /// Forget every entry. The scopes and the bounds are unchanged.
    void clear() noexcept;

private:
    struct Entry {
        std::string server_id{};
        UpstreamCatalog catalog{};
        std::chrono::steady_clock::time_point expires_at{};
    };

    UpstreamCatalogCacheOptions options;
    /// Oldest admission first, so eviction drops the entry that has been
    /// held the longest.
    std::vector<Entry> entries;
    mutable std::mutex mutex;
};

} // namespace cch::mcp
