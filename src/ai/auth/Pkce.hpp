#pragma once

#include <cch/support/Error.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace cch::ai::auth {

/// PKCE S256 pair: a 32-byte base64url code verifier and its SHA-256 base64url
/// challenge, matching pi's `generatePKCE` (auth/oauth/pkce.ts).
struct PkcePair {
    std::string verifier{};
    std::string challenge{};
};

/// Base64url (no padding) encode of raw bytes, pi's `base64urlEncode`.
[[nodiscard]] std::string base64url_encode(std::string_view bytes);

/// Random 32-byte verifier base64url-encoded plus its SHA-256 challenge.
[[nodiscard]] support::Expected<PkcePair> generate_pkce();

/// Random 16-byte state as a 32-character lowercase hex string (pi
/// `createState`).
[[nodiscard]] support::Expected<std::string> create_oauth_state();

/// URLSearchParams-style query encoding: unreserved (`A-Za-z0-9*-._`) and
/// alphanumeric pass through, space becomes `+`, everything else `%XX` with
/// uppercase UTF-8 bytes. Byte-identical to pi's `new URLSearchParams` output.
[[nodiscard]] std::string url_query_encode(std::string_view value);

/// Inverse of `url_query_encode`: `+` becomes space, `%XX` becomes one byte.
[[nodiscard]] std::string url_query_decode(std::string_view value);

/// Parse a `key=value&key2=value2` query into decoded pairs. Duplicate keys
/// keep the first value, matching pi's `URLSearchParams.get` semantics.
[[nodiscard]] std::map<std::string, std::string, std::less<>> parse_query_pairs(
    std::string_view query);

/// Parsed manual authorization input (pi `parseAuthorizationInput`).
struct AuthorizationInput {
    std::optional<std::string> code{std::nullopt};
    std::optional<std::string> state{std::nullopt};
};

/// Accepts a full redirect URL, `code#state`, a query string containing
/// `code=`, or a bare code, in pi's exact fallback order.
[[nodiscard]] AuthorizationInput parse_authorization_input(std::string_view input);

/// Extract the non-empty `chatgpt_account_id` from the unverified JWT claim
/// `https://api.openai.com/auth` (three-part split, base64url decode, JSON
/// parse). Fails when the token is not a three-part JWT or the claim is
/// missing/empty, matching pi's `getAccountId` + `credentialsFromToken`.
[[nodiscard]] support::Expected<std::string> extract_account_id(std::string_view access_token);

} // namespace cch::ai::auth
