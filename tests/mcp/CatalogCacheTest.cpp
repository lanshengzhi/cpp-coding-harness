// The per-Upstream tool-catalog cache driven through the one injected
// transport seam (issue #848): the `ttlMs`/`cacheScope` hints an Upstream
// provides, a warm cache that costs a repeated session nothing, a background
// refresh that never makes a call wait, and scope isolation.
//
// No case here reaches past the transport. What the Upstream was asked for is
// read back off the scripted transport, and the catalogs a case compares are
// the values the connection handed its caller. The cache's own capacity and
// admission rules are reached through its Owner Interface, which is a
// dependency-supplied value type and not a seam.

#include <cch/mcp/UpstreamCatalogCache.hpp>
#include <cch/mcp/UpstreamConnection.hpp>
#include "support/ScriptedMcpTransport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

constexpr std::string_view kEndpoint{"https://upstream.invalid/mcp"};
/// A day, the longest an Upstream's own freshness hint can keep an entry.
constexpr double kMaxFreshnessHintMs = 24.0 * 60.0 * 60.0 * 1000.0;

/// One connection under test with the catalog cache it shares, the transport
/// it drives, and the timer its close and ladder run on.
struct Fixture {
    std::shared_ptr<tests::ScriptedMcpTransport> transport = std::make_shared<tests::ScriptedMcpTransport>();
    tests::ScriptedMcpDelay clock;
    std::shared_ptr<mcp::UpstreamCatalogCache> cache;
    mcp::UpstreamConnection connection;

    explicit Fixture(std::string scope = std::string(mcp::kDefaultCatalogCacheScope))
        : cache(std::make_shared<mcp::UpstreamCatalogCache>(mcp::UpstreamCatalogCacheOptions{.scope = std::move(scope)})),
          connection("executor",
                  transport,
                  mcp::UpstreamConnectionOptions{
                          .url = std::string(kEndpoint),
                          .delay = [this](std::chrono::milliseconds delay,
                                           std::stop_token stop_token) { return clock.request(delay, stop_token); },
                          .catalog_cache = cache,
                  }) {}

    /// A conforming Upstream whose era probe answers and whose `tools/list`
    /// reports one tool per name, in exactly the order given, fresh for
    /// `ttl_ms` and reusable in `cache_scope`.
    void answer_catalog(const std::vector<std::string>& tool_names,
            std::optional<double> ttl_ms,
            std::optional<std::string> cache_scope = std::nullopt) {
        transport->answer("server/discover", {.result = tests::discover_result()});
        transport->answer("tools/call", {.result = tests::tool_call_result(support::JsonValue::array_t{})});
        transport->answer("tools/list",
                {.result = tests::tool_list_result(tool_entries(tool_names), std::nullopt, ttl_ms, std::move(cache_scope))});
    }

    [[nodiscard]] static std::vector<support::JsonValue> tool_entries(const std::vector<std::string>& names) {
        std::vector<support::JsonValue> entries;
        entries.reserve(names.size());
        for (const auto& name : names) {
            entries.push_back(tests::tool_entry(name));
        }
        return entries;
    }

    /// A connection that has completed its era probe, so a listing is about
    /// the catalog and not about the connection.
    void connect() {
        transport->answer("server/discover", {.result = tests::discover_result()});
        transport->answer("tools/call", {.result = tests::tool_call_result(support::JsonValue::array_t{})});
        const auto probed = tests::drive(connection.connect());
        REQUIRE(probed.has_value());
    }

    [[nodiscard]] std::size_t listings() const { return transport->request_count("tools/list"); }
};

[[nodiscard]] std::vector<std::string> names_of(const mcp::UpstreamCatalog& catalog) {
    std::vector<std::string> names;
    names.reserve(catalog.tools.size());
    for (const auto& tool : catalog.tools) {
        names.push_back(tool.name);
    }
    return names;
}

[[nodiscard]] mcp::UpstreamCatalog catalog_of(std::vector<std::string> names,
        std::chrono::milliseconds freshness = std::chrono::minutes{5},
        std::string scope = std::string(mcp::kDefaultCatalogCacheScope)) {
    mcp::UpstreamCatalog catalog;
    for (auto& name : names) {
        catalog.tools.push_back(mcp::UpstreamToolDescriptor{.name = std::move(name)});
    }
    catalog.freshness = freshness;
    catalog.cache_scope = std::move(scope);
    return catalog;
}

} // namespace

TEST_CASE("a warm cache answers a second listing without asking the Upstream again",
        "[mcp][catalog][cache][issue848][spec]") {
    Fixture fixture;
    fixture.answer_catalog({"read_issue", "write_file"}, 60000.0);
    fixture.connect();

    const auto first = tests::drive(fixture.connection.list_tools());
    REQUIRE(first.has_value());
    CHECK(fixture.listings() == 1);

    const auto second = tests::drive(fixture.connection.list_tools());
    REQUIRE(second.has_value());
    // The whole of story 17: the second listing of the same catalog, from the
    // same connection, costs the Upstream nothing at all.
    CHECK(fixture.listings() == 1);
    CHECK(second->tools.size() == first->tools.size());
    CHECK(fixture.cache->size() == 1);
}

TEST_CASE("a connection that has not connected yet is answered from a warm cache without waiting",
        "[mcp][catalog][cache][startup][issue848][spec]") {
    Fixture fixture;
    fixture.answer_catalog({"read_issue"}, 60000.0);
    fixture.connect();
    REQUIRE(tests::drive(fixture.connection.list_tools()).has_value());
    const auto before = fixture.transport->request_count();

    // A second connection over the same cache, before it has probed anything:
    // the era is per connection, but the catalog is not.
    mcp::UpstreamConnection later("executor",
            fixture.transport,
            mcp::UpstreamConnectionOptions{
                    .url = std::string(kEndpoint),
                    .delay = [&fixture](std::chrono::milliseconds delay,
                             std::stop_token stop_token) { return fixture.clock.request(delay, stop_token); },
                    .catalog_cache = fixture.cache,
            });
    CHECK(later.status() == mcp::UpstreamConnectionStatus::Pending);

    const auto catalog = tests::drive(later.list_tools());
    REQUIRE(catalog.has_value());
    CHECK(names_of(*catalog) == std::vector<std::string>{"read_issue"});
    // No era probe and no walk: a pending connection is served from the cache
    // rather than made to wait for one.
    CHECK(fixture.transport->request_count() == before);
}

TEST_CASE("a cached catalog keeps the server's own tools/list order",
        "[mcp][catalog][cache][order][issue848][spec]") {
    Fixture fixture;
    // Deliberately not alphabetical: the catalog is whatever the server said,
    // and neither the cache nor a refresh is allowed to sort it.
    const std::vector<std::string> declared{"zeta", "alpha", "mu", "beta"};
    fixture.answer_catalog(declared, 0.0);
    fixture.connect();

    const auto first = tests::drive(fixture.connection.list_tools());
    REQUIRE(first.has_value());
    CHECK(names_of(*first) == declared);

    // The `ttlMs: 0` entry is already stale, so this listing is served from
    // it and a refresh runs behind the caller's back; the refreshed catalog
    // replaces the entry in the same server order.
    const auto second = tests::drive(fixture.connection.list_tools());
    REQUIRE(second.has_value());
    CHECK(names_of(*second) == declared);
    const auto stored = fixture.cache->lookup("executor");
    REQUIRE(stored.has_value());
    CHECK(names_of(stored->catalog) == declared);
    CHECK(fixture.listings() == 2);
}

TEST_CASE("an expired entry is served immediately and the refresh replaces it",
        "[mcp][catalog][cache][refresh][issue848][spec]") {
    Fixture fixture;
    fixture.transport->answer("server/discover", {.result = tests::discover_result()});
    fixture.transport->answer("tools/call", {.result = tests::tool_call_result(support::JsonValue::array_t{})});
    // The first answer declares no freshness, so the walk is not cached; the
    // second declares an already-expired one, and every later answer declares
    // a fresh catalog in a new order.
    std::size_t served = 0;
    fixture.transport->answer_with("tools/list", [&served](const support::JsonValue&) {
        ++served;
        if (served == 1) {
            return tests::ScriptedMcpAnswer{.result = tests::tool_list_result(Fixture::tool_entries({"before"}))};
        }
        if (served == 2) {
            return tests::ScriptedMcpAnswer{
                    .result = tests::tool_list_result(Fixture::tool_entries({"stale_one", "stale_two"}), std::nullopt, 0.0)};
        }
        return tests::ScriptedMcpAnswer{
                .result = tests::tool_list_result(Fixture::tool_entries({"fresh_one", "fresh_two"}), std::nullopt, 60000.0)};
    });
    fixture.connect();

    REQUIRE(tests::drive(fixture.connection.list_tools()).has_value());
    CHECK(fixture.cache->size() == 0); // no hint, no reuse

    const auto second = tests::drive(fixture.connection.list_tools());
    REQUIRE(second.has_value());
    CHECK(names_of(*second) == std::vector<std::string>{"stale_one", "stale_two"});
    // That walk's own hint was already expired, so the entry it left behind
    // is stale. The listing that reads it is the one that refreshes.
    const auto expired = fixture.cache->lookup("executor");
    REQUIRE(expired.has_value());
    CHECK(expired->stale);

    // The next listing is served from the expired entry immediately and the
    // refresh lands behind it, so the entry becomes the fresh catalog in its
    // own server order.
    const auto refreshed = tests::drive(fixture.connection.list_tools());
    REQUIRE(refreshed.has_value());
    CHECK(names_of(*refreshed) == std::vector<std::string>{"stale_one", "stale_two"});
    const auto stored = fixture.cache->lookup("executor");
    REQUIRE(stored.has_value());
    CHECK_FALSE(stored->stale);
    CHECK(names_of(stored->catalog) == std::vector<std::string>{"fresh_one", "fresh_two"});

    const auto warm = tests::drive(fixture.connection.list_tools());
    REQUIRE(warm.has_value());
    CHECK(names_of(*warm) == std::vector<std::string>{"fresh_one", "fresh_two"});
    CHECK(fixture.listings() == 3);
}

TEST_CASE("a refresh that never answers does not delay a listing or a tool call",
        "[mcp][catalog][cache][refresh][issue848][spec]") {
    Fixture fixture;
    fixture.transport->answer("server/discover", {.result = tests::discover_result()});
    fixture.transport->answer("tools/call", {.result = tests::tool_call_result(support::JsonValue::array_t{})});
    fixture.transport->answer_with("tools/list", [](const support::JsonValue&) {
        return tests::ScriptedMcpAnswer{
                .result = tests::tool_list_result(Fixture::tool_entries({"read_issue"}), std::nullopt, 0.0)};
    });
    fixture.connect();
    REQUIRE(tests::drive(fixture.connection.list_tools()).has_value());
    CHECK(fixture.listings() == 1);

    // The Upstream takes the refresh and goes silent, which is the state a
    // stale entry exists to survive.
    fixture.transport->hold("tools/list");

    const auto stale = tests::drive(fixture.connection.list_tools());
    REQUIRE(stale.has_value());
    CHECK(names_of(*stale) == std::vector<std::string>{"read_issue"});

    // A `tools/call` on the same connection is unaffected by the refresh that
    // is still outstanding: the cache never puts an Upstream on a caller's
    // critical path.
    const auto called = tests::drive(fixture.connection.call_tool(
            mcp::UpstreamToolCall{.tool = mcp::UpstreamToolDescriptor{.name = "read_issue"},
                    .arguments = support::JsonValue::object_t{}}));
    REQUIRE(called.has_value());
    CHECK_FALSE(called->is_error);
    CHECK(fixture.transport->request_count("tools/call") == 1);
    CHECK(fixture.listings() == 2);

    // And the close reaches quiescence with the outstanding refresh, because
    // the refresh is an admitted operation like any other.
    const auto closed = tests::drive(fixture.connection.close());
    REQUIRE(closed.has_value());
    CHECK(closed->within_bound);
    CHECK(closed->abandoned_operations == 0);
}

TEST_CASE("an Upstream that expires its catalog on every hint costs one refresh at a time",
        "[mcp][catalog][cache][refresh][issue848][spec]") {
    Fixture fixture;
    fixture.transport->answer("server/discover", {.result = tests::discover_result()});
    fixture.transport->answer("tools/list", {.result = tests::tool_list_result(Fixture::tool_entries({"read_issue"}),
                                                                                 std::nullopt,
                                                                                 0.0)});
    fixture.connect();
    REQUIRE(tests::drive(fixture.connection.list_tools()).has_value());
    CHECK(fixture.listings() == 1);

    // Every later walk is held open, so the first refresh never lands. Ten
    // more listings must not become ten more walks.
    fixture.transport->hold("tools/list");
    for (int lookup = 0; lookup < 10; ++lookup) {
        const auto catalog = tests::drive(fixture.connection.list_tools());
        REQUIRE(catalog.has_value());
        CHECK(names_of(*catalog) == std::vector<std::string>{"read_issue"});
    }
    CHECK(fixture.listings() == 2);
}

TEST_CASE("a refresh re-applies the defensive catalog limits and leaves the cached entry alone",
        "[mcp][catalog][cache][limits][issue848][spec]") {
    Fixture fixture;
    fixture.transport->answer("server/discover", {.result = tests::discover_result()});
    std::size_t served = 0;
    fixture.transport->answer_with("tools/list", [&served](const support::JsonValue&) {
        ++served;
        if (served == 1) {
            return tests::ScriptedMcpAnswer{.result = tests::tool_list_result(Fixture::tool_entries({"kept"}),
                                                                                std::nullopt,
                                                                                0.0)};
        }
        // A refresh that now walks past the per-server tool cap, page by
        // page, exactly as a cold fetch that over-reports would.
        const std::size_t page_size = 2600;
        std::vector<support::JsonValue> entries;
        for (std::size_t index = 0; index < page_size; ++index) {
            entries.push_back(tests::tool_entry("tool_" + std::to_string(served) + "_" + std::to_string(index)));
        }
        const std::optional<std::string> cursor = served == 2 ? std::optional<std::string>{"next"} : std::nullopt;
        return tests::ScriptedMcpAnswer{
                .result = tests::tool_list_result(std::move(entries), cursor, 60000.0)};
    });
    fixture.connect();
    REQUIRE(tests::drive(fixture.connection.list_tools()).has_value());
    CHECK(names_of(*tests::drive(fixture.connection.list_tools())) == std::vector<std::string>{"kept"});

    // The refresh failed at the same 5,000-tool cap a cold fetch fails at, and
    // the entry the caller was already served is still the one it holds: a
    // failed refresh replaces nothing. The connection stays `connected` too,
    // because an Upstream that over-reports is a catalog problem and not
    // transport evidence the reconnect ladder may act on.
    const auto stored = fixture.cache->lookup("executor");
    REQUIRE(stored.has_value());
    CHECK(names_of(stored->catalog) == std::vector<std::string>{"kept"});
    CHECK(fixture.connection.status() == mcp::UpstreamConnectionStatus::Connected);
}

TEST_CASE("an Upstream that declares no freshness hint is never served from the cache",
        "[mcp][catalog][cache][hints][issue848][spec]") {
    Fixture fixture;
    fixture.answer_catalog({"read_issue"}, std::nullopt);
    fixture.answer_catalog({"read_issue"}, std::nullopt);
    fixture.connect();

    REQUIRE(tests::drive(fixture.connection.list_tools()).has_value());
    REQUIRE(tests::drive(fixture.connection.list_tools()).has_value());
    // The Upstream never said its catalog may be reused, so it is not: the
    // cache is a shortcut the Upstream asks for, never a default.
    CHECK(fixture.listings() == 2);
    CHECK(fixture.cache->size() == 0);
}

TEST_CASE("a connection with no cache at all still walks the catalog every time",
        "[mcp][catalog][cache][hints][issue848][spec]") {
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    tests::ScriptedMcpDelay clock;
    // No `catalog_cache` in the options at all: the uncached connection is
    // the behaviour the host had before the cache existed, and it is what a
    // null cache means rather than a cache that silently stopped working.
    mcp::UpstreamConnection connection("executor",
            transport,
            mcp::UpstreamConnectionOptions{
                    .url = std::string(kEndpoint),
                    .delay = [&clock](std::chrono::milliseconds delay,
                                   std::stop_token stop_token) { return clock.request(delay, stop_token); },
            });
    transport->answer("server/discover", {.result = tests::discover_result()});
    transport->answer("tools/list",
            {.result = tests::tool_list_result(Fixture::tool_entries({"read_issue"}), std::nullopt, 60000.0)});
    REQUIRE(tests::drive(connection.connect()).has_value());

    REQUIRE(tests::drive(connection.list_tools()).has_value());
    REQUIRE(tests::drive(connection.list_tools()).has_value());
    CHECK(transport->request_count("tools/list") == 2);
}

TEST_CASE("an entry in one scope is never served to a cache in another",
        "[mcp][catalog][cache][scope][issue848][spec]") {
    auto session_scoped = std::make_shared<mcp::UpstreamCatalogCache>(
            mcp::UpstreamCatalogCacheOptions{.scope = "session"});
    auto process_scoped = std::make_shared<mcp::UpstreamCatalogCache>(
            mcp::UpstreamCatalogCacheOptions{.scope = "process"});

    CHECK(session_scoped->lookup("executor") == std::nullopt);
    REQUIRE(session_scoped->admit("executor", catalog_of({"read_issue"})).has_value());
    CHECK(session_scoped->size() == 1);

    // The same catalog in the other scope: not this cache's entry, and never
    // served from it.
    CHECK(process_scoped->lookup("executor") == std::nullopt);
    CHECK(process_scoped->size() == 0);
}

TEST_CASE("an Upstream that declares another scope is not stored in this scope's cache",
        "[mcp][catalog][cache][scope][issue848][spec]") {
    Fixture fixture;
    // The Upstream says its catalog may be reused process-wide. A cache that
    // reads the session scope will not hold it, because serving it here is
    // exactly the cross-contamination the scope exists to prevent.
    fixture.answer_catalog({"read_issue"}, 60000.0, "process");
    fixture.connect();

    REQUIRE(tests::drive(fixture.connection.list_tools()).has_value());
    CHECK(fixture.cache->size() == 0);
    REQUIRE(tests::drive(fixture.connection.list_tools()).has_value());
    CHECK(fixture.listings() == 2);
}

TEST_CASE("a connection whose Upstream was turned off is answered from the cache by nothing",
        "[mcp][catalog][cache][scope][issue848][spec]") {
    Fixture fixture;
    fixture.answer_catalog({"read_issue"}, 60000.0);
    fixture.connect();
    REQUIRE(tests::drive(fixture.connection.list_tools()).has_value());
    REQUIRE(fixture.cache->size() == 1);

    fixture.connection.disable("the user turned this Upstream off");
    const auto listed = tests::drive(fixture.connection.list_tools());
    REQUIRE_FALSE(listed.has_value());
    CHECK(listed.error().code == support::ErrorCode::Cancelled);
    // The entry is still there for a connection that is allowed to use it;
    // what was withdrawn is this connection's right to read it.
    CHECK(fixture.cache->size() == 1);
    CHECK(fixture.listings() == 1);
}

TEST_CASE("a catalog the cache would not admit is refused rather than held",
        "[mcp][catalog][cache][untrusted][issue848][spec]") {
    mcp::UpstreamCatalogCache cache;

    const auto over_cap = cache.admit("executor",
            catalog_of(std::vector<std::string>(5001, "read_issue")));
    REQUIRE_FALSE(over_cap.has_value());
    CHECK(over_cap.error().code == support::ErrorCode::ResourceLimit);
    CHECK(cache.size() == 0);

    // A cached catalog is untrusted whatever produced it, so the walk-shaped
    // rules cannot be re-run against it but the value-shaped ones are.
    const auto duplicate = cache.admit("executor", catalog_of({"read_issue", "read_issue"}));
    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error().code == support::ErrorCode::Validation);
    CHECK(duplicate.error().detail.find("read_issue") == std::string::npos);
    CHECK(cache.size() == 0);

    const auto fresh = cache.admit("executor", catalog_of({"read_issue"}));
    CHECK(fresh.has_value());
    CHECK(cache.size() == 1);
}

TEST_CASE("the cache is bounded in entries and drops the oldest one at the bound",
        "[mcp][catalog][cache][limits][issue848][spec]") {
    mcp::UpstreamCatalogCache cache;
    for (std::size_t index = 0; index < 64; ++index) {
        const auto server = "server-" + std::to_string(index);
        REQUIRE(cache.admit(server, catalog_of({"read_issue"})).has_value());
    }
    CHECK(cache.size() == 64);
    CHECK(cache.lookup("server-0").has_value());

    // The 65th server is admitted — bounding the cache must never silently
    // disable caching for a server the user still has — and the entry that
    // has been held the longest is the one dropped.
    REQUIRE(cache.admit("server-64", catalog_of({"write_file"})).has_value());
    CHECK(cache.size() == 64);
    CHECK(cache.lookup("server-0") == std::nullopt);
    CHECK(cache.lookup("server-1").has_value());
    CHECK(cache.lookup("server-64").has_value());

    // Admitting a server it already holds replaces its entry rather than
    // growing the cache.
    REQUIRE(cache.admit("server-64", catalog_of({"write_file", "read_file"})).has_value());
    CHECK(cache.size() == 64);
    const auto replaced = cache.lookup("server-64");
    REQUIRE(replaced.has_value());
    CHECK(names_of(replaced->catalog) == (std::vector<std::string>{"write_file", "read_file"}));
}

TEST_CASE("a freshness hint longer than the host admits is clamped rather than refused",
        "[mcp][catalog][cache][limits][issue848][spec]") {
    Fixture fixture;
    fixture.answer_catalog({"read_issue"}, 1.0e12);
    fixture.connect();

    const auto catalog = tests::drive(fixture.connection.list_tools());
    REQUIRE(catalog.has_value());
    REQUIRE(catalog->freshness.has_value());
    CHECK(static_cast<double>(catalog->freshness->count()) == kMaxFreshnessHintMs);
    // Clamped, not refused: the walk still produced a usable catalog, and the
    // Upstream still gets to have its catalog.
    CHECK(fixture.cache->size() == 1);
}

TEST_CASE("a tools/list result whose freshness hint or reuse scope is unusable is rejected",
        "[mcp][catalog][cache][hints][issue848][spec]") {
    Fixture fixture;
    fixture.transport->answer("server/discover", {.result = tests::discover_result()});
    fixture.connect();

    const std::vector<std::pair<support::JsonValue, std::string>> unusable{
            {support::JsonValue("soon"), "ttlMs is a string"},
            {support::JsonValue(-1.0), "ttlMs is negative"},
            {support::JsonValue(true), "ttlMs is a boolean"},
            {support::JsonValue(3), "cacheScope is a number"},
            {support::JsonValue(""), "cacheScope is empty"},
    };
    for (const auto& [value, what] : unusable) {
        support::JsonValue::object_t result{
                {"tools", support::JsonValue::array_t{}},
                {std::string(what.substr(0, what.find(' '))), value},
        };
        fixture.transport->answer("tools/list", {.result = support::JsonValue(std::move(result))});
        const auto listed = tests::drive(fixture.connection.list_tools());
        CAPTURE(what);
        REQUIRE_FALSE(listed.has_value());
        CHECK(listed.error().code == support::ErrorCode::Validation);
    }
    CHECK(fixture.cache->size() == 0);
}

TEST_CASE("a hint on a page that still carries a nextCursor describes no catalog and is not read",
        "[mcp][catalog][cache][hints][issue848][spec]") {
    Fixture fixture;
    fixture.transport->answer("server/discover", {.result = tests::discover_result()});
    fixture.transport->answer_with("tools/list", [](const support::JsonValue& params) {
        const auto& object = params.get_object();
        if (object.find("cursor") == object.end()) {
            return tests::ScriptedMcpAnswer{.result = tests::tool_list_result(Fixture::tool_entries({"first"}),
                                                                               "page-2",
                                                                               60000.0,
                                                                               "session")};
        }
        // The completing page carries no hint at all, so the catalog has none
        // even though an earlier page advertised one.
        return tests::ScriptedMcpAnswer{.result = tests::tool_list_result(Fixture::tool_entries({"second"}))};
    });
    fixture.connect();

    const auto catalog = tests::drive(fixture.connection.list_tools());
    REQUIRE(catalog.has_value());
    CHECK(names_of(*catalog) == (std::vector<std::string>{"first", "second"}));
    CHECK_FALSE(catalog->freshness.has_value());
    CHECK(fixture.cache->size() == 0);
}
