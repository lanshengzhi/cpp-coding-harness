#pragma once

#include <cch/mcp/UpstreamOAuth.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::mcp::www_authenticate {

/// One challenge exactly as the header spelled it, with every `auth-param`
/// kept. Parameter names are lowercased because HTTP field names and
/// auth-param names are case-insensitive; the first occurrence of a repeated
/// name wins, and a `token68` credential is kept in `token68` rather than
/// being mistaken for a parameter.
struct RawChallenge {
    std::string scheme{};
    std::vector<std::pair<std::string, std::string>> params{};
    /// The `token68` credential a scheme may carry instead of parameters
    /// (`Basic dGVzdA==`). Empty for a challenge that carried parameters.
    std::string token68{};
    /// Whether this challenge parsed. A malformed challenge is dropped whole
    /// rather than half-parsed, so a caller never acts on a partial
    /// authorization requirement.
    bool valid{false};
};

/// Parse one `WWW-Authenticate` field value into its challenges (RFC 9110
/// §11.6.1).
///
/// The grammar is the general one: several comma-separated challenges, an
/// optional `token68` per challenge, quoted-string values that may contain
/// commas and escaped quotes, `auth-param`s with no value, and any spelling
/// of the scheme or a parameter name. A malformed field contributes the
/// challenges that preceded it and no more; an empty or wholly malformed
/// field contributes none. Nothing here throws, and no allocation failure is
/// caught.
[[nodiscard]] std::vector<RawChallenge> parse(std::string_view header);

/// The first `Bearer` challenge in `header`, reduced to what the authorization
/// flow reads. `std::nullopt` when the header carried no `Bearer` challenge
/// this build can act on — including when the only challenges were
/// well-formed schemes this build does not implement, and when a `Bearer`
/// challenge was malformed.
///
/// The reduction never invents a value: a `resource_metadata` that is present
/// but empty is carried as an empty URL, and `error` may appear more than
/// once, which is the only member that accumulates.
[[nodiscard]] std::optional<AuthorizationChallenge> first_bearer_challenge(std::string_view header);

/// The same reduction for a challenge this build already parsed, used where
/// the parser runs once and several readers consume the result.
[[nodiscard]] AuthorizationChallenge reduce(const RawChallenge& challenge);

} // namespace cch::mcp::www_authenticate
