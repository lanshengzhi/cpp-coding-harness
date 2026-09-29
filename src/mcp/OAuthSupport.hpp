#pragma once

#include <cch/support/JsonValue.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::mcp::oauth {

/// The one PKCE pair of one authorization request (RFC 7636). `challenge` is
/// the `S256` transform of `verifier`; the verifier itself never leaves this
/// package and is the only value that can redeem the code.
struct PkcePair {
    std::string verifier{};
    std::string challenge{};
};

/// Mint one PKCE pair and one OAuth `state` value, from the platform CSPRNG
/// (`RAND_bytes`). A request that cannot be given unpredictable values is not
/// sent: `std::nullopt` means the flow fails closed rather than authorizing
/// with a guessable verifier or state.
[[nodiscard]] std::optional<PkcePair> generate_pkce();

/// One unpredictable, URL-safe value of `bytes` random bytes. Exposed for the
/// OAuth `state`, which is not a PKCE artifact.
[[nodiscard]] std::optional<std::string> random_url_safe(std::size_t bytes);

/// Percent-encode one component for a query string or a form body
/// (unreserved characters only, space as `%20`).
[[nodiscard]] std::string url_encode(std::string_view value);

/// One `application/x-www-form-urlencoded` body from ordered parameters.
[[nodiscard]] std::string form_encode(const std::vector<std::pair<std::string, std::string>>& parameters);

/// Append `parameters` to `base`'s query, preserving whatever query `base`
/// already carries. An empty parameter list returns `base` unchanged.
[[nodiscard]] std::string append_query(
        std::string_view base, const std::vector<std::pair<std::string, std::string>>& parameters);

/// The `origin` of an absolute `https://host[:port][/path]` URL — scheme,
/// host, and port — which is the base every `.well-known` discovery document
/// hangs from. Empty when `url` is not absolute.
[[nodiscard]] std::string origin_of(std::string_view url);

/// Every `name=value` pair of a query string, percent-decoded. A repeated
/// name contributes every occurrence in order, because the authorization
/// response's `iss` and `error` may both appear more than once.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> parse_query(std::string_view query);

/// The first value of `name` in a parsed query, or the empty string.
[[nodiscard]] std::string query_value(
        const std::vector<std::pair<std::string, std::string>>& query, std::string_view name);

/// The string member `name` of a JSON object, or `std::nullopt` when the value
/// is absent, is not an object, or does not carry a string there. A
/// non-string member reads as absent rather than as a guess.
[[nodiscard]] std::optional<std::string> string_member(const cch::support::JsonValue& value, std::string_view name);

/// The number member `name` as a whole number, or `std::nullopt` when it is
/// absent, fractional, or out of `int64` range.
[[nodiscard]] std::optional<std::int64_t> integer_member(const cch::support::JsonValue& value, std::string_view name);

/// The string-array member `name`, ignoring members that are not strings.
[[nodiscard]] std::vector<std::string> string_array_member(const cch::support::JsonValue& value, std::string_view name);

/// The space-delimited `scope` of a token response, split into its scopes.
[[nodiscard]] std::vector<std::string> split_scopes(std::string_view scope);

/// The OAuth `error` of a JSON error body: the `error` member, or
/// `error_description` when only that one is present.
[[nodiscard]] std::string error_of(const cch::support::JsonValue& value);

} // namespace cch::mcp::oauth
