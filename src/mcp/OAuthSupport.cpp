#include "mcp/OAuthSupport.hpp"

#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::mcp::oauth {
namespace {

/// The base64url alphabet (RFC 4648 §5) without padding, which is the only
/// encoding PKCE and the OAuth `state` are defined in.
constexpr std::string_view kBase64UrlAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

[[nodiscard]] std::string base64url_encode(const unsigned char* data, std::size_t size) {
    std::string encoded;
    encoded.reserve(((size + 2) / 3) * 4);
    std::size_t index = 0;
    while (index + 3 <= size) {
        const auto group = (static_cast<unsigned int>(data[index]) << 16U) |
                           (static_cast<unsigned int>(data[index + 1]) << 8U) |
                           static_cast<unsigned int>(data[index + 2]);
        encoded.push_back(kBase64UrlAlphabet[(group >> 18U) & 0x3FU]);
        encoded.push_back(kBase64UrlAlphabet[(group >> 12U) & 0x3FU]);
        encoded.push_back(kBase64UrlAlphabet[(group >> 6U) & 0x3FU]);
        encoded.push_back(kBase64UrlAlphabet[group & 0x3FU]);
        index += 3;
    }
    const auto remaining = size - index;
    if (remaining == 1) {
        const auto group = static_cast<unsigned int>(data[index]) << 16U;
        encoded.push_back(kBase64UrlAlphabet[(group >> 18U) & 0x3FU]);
        encoded.push_back(kBase64UrlAlphabet[(group >> 12U) & 0x3FU]);
    } else if (remaining == 2) {
        const auto group =
                (static_cast<unsigned int>(data[index]) << 16U) | (static_cast<unsigned int>(data[index + 1]) << 8U);
        encoded.push_back(kBase64UrlAlphabet[(group >> 18U) & 0x3FU]);
        encoded.push_back(kBase64UrlAlphabet[(group >> 12U) & 0x3FU]);
        encoded.push_back(kBase64UrlAlphabet[(group >> 6U) & 0x3FU]);
    }
    return encoded;
}

[[nodiscard]] int hex_value(unsigned char byte) noexcept {
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }
    if (byte >= 'A' && byte <= 'F') {
        return byte - 'A' + 10;
    }
    return -1;
}

/// Percent-decode one query or form component. A `%` that does not begin a
/// two-hex-digit escape is kept verbatim, and a `+` becomes a space, which is
/// what `application/x-www-form-urlencoded` means.
[[nodiscard]] std::string percent_decode(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '+') {
            decoded.push_back(' ');
            continue;
        }
        if (value[index] == '%' && index + 2 < value.size()) {
            const auto high = hex_value(static_cast<unsigned char>(value[index + 1]));
            const auto low = hex_value(static_cast<unsigned char>(value[index + 2]));
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        decoded.push_back(value[index]);
    }
    return decoded;
}

} // namespace

std::optional<std::string> random_url_safe(std::size_t bytes) {
    if (bytes == 0) {
        return std::nullopt;
    }
    std::vector<unsigned char> buffer(bytes, 0U);
    if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1) {
        return std::nullopt;
    }
    return base64url_encode(buffer.data(), buffer.size());
}

std::optional<PkcePair> generate_pkce() {
    // RFC 7636 §4.1: the verifier is 43-128 characters of the unreserved set,
    // which 32 random bytes base64url-encoded (43 characters) satisfies.
    auto verifier = random_url_safe(32);
    if (!verifier.has_value()) {
        return std::nullopt;
    }
    const auto digest = SHA256(reinterpret_cast<const unsigned char*>(verifier->data()), verifier->size(), nullptr);
    if (digest == nullptr) {
        return std::nullopt;
    }
    return PkcePair{.verifier = *verifier, .challenge = base64url_encode(digest, SHA256_DIGEST_LENGTH)};
}

std::string url_encode(std::string_view value) {
    static constexpr std::string_view kHexDigits{"0123456789ABCDEF"};
    std::string encoded;
    encoded.reserve(value.size());
    for (const auto byte : value) {
        const auto raw = static_cast<unsigned char>(byte);
        if (std::isalnum(raw) != 0 || raw == '-' || raw == '_' || raw == '.' || raw == '~') {
            encoded.push_back(byte);
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(kHexDigits[(raw >> 4U) & 0x0FU]);
        encoded.push_back(kHexDigits[raw & 0x0FU]);
    }
    return encoded;
}

std::string form_encode(const std::vector<std::pair<std::string, std::string>>& parameters) {
    std::string body;
    for (const auto& [name, value] : parameters) {
        if (!body.empty()) {
            body.push_back('&');
        }
        body += url_encode(name);
        body.push_back('=');
        body += url_encode(value);
    }
    return body;
}

std::string append_query(std::string_view base, const std::vector<std::pair<std::string, std::string>>& parameters) {
    if (parameters.empty()) {
        return std::string{base};
    }
    std::string url{base};
    url += (url.find('?') == std::string_view::npos) ? '?' : '&';
    url += form_encode(parameters);
    return url;
}

std::string origin_of(std::string_view url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) {
        return {};
    }
    const auto authority_start = scheme_end + 3;
    const auto authority_end = url.find_first_of("/?#", authority_start);
    const auto authority = url.substr(authority_start,
            authority_end == std::string_view::npos ? std::string_view::npos : authority_end - authority_start);
    if (authority.empty()) {
        return {};
    }
    return std::string{url.substr(0, authority_end == std::string_view::npos ? url.size() : authority_end)};
}

std::vector<std::pair<std::string, std::string>> parse_query(std::string_view query) {
    std::vector<std::pair<std::string, std::string>> pairs;
    std::size_t start = 0;
    while (start <= query.size()) {
        const auto separator = query.find_first_of("&;", start);
        const auto end = separator == std::string_view::npos ? query.size() : separator;
        const auto field = query.substr(start, end - start);
        if (!field.empty()) {
            const auto equals = field.find('=');
            if (equals == std::string_view::npos) {
                pairs.emplace_back(percent_decode(field), std::string{});
            } else {
                pairs.emplace_back(percent_decode(field.substr(0, equals)), percent_decode(field.substr(equals + 1)));
            }
        }
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1;
    }
    return pairs;
}

std::string query_value(const std::vector<std::pair<std::string, std::string>>& query, std::string_view name) {
    for (const auto& [key, value] : query) {
        if (key == name) {
            return value;
        }
    }
    return {};
}

std::optional<std::string> string_member(const cch::support::JsonValue& value, std::string_view name) {
    const auto* object = value.get_if<cch::support::JsonValue::object_t>();
    if (object == nullptr) {
        return std::nullopt;
    }
    const auto found = object->find(std::string{name});
    if (found == object->end()) {
        return std::nullopt;
    }
    const auto* text = found->second.get_if<std::string>();
    if (text == nullptr) {
        return std::nullopt;
    }
    return *text;
}

std::optional<std::int64_t> integer_member(const cch::support::JsonValue& value, std::string_view name) {
    const auto* object = value.get_if<cch::support::JsonValue::object_t>();
    if (object == nullptr) {
        return std::nullopt;
    }
    const auto found = object->find(std::string{name});
    if (found == object->end()) {
        return std::nullopt;
    }
    const auto* number = found->second.get_if<double>();
    if (number == nullptr) {
        return std::nullopt;
    }
    if (!std::isfinite(*number) || std::trunc(*number) != *number) {
        return std::nullopt;
    }
    if (*number < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
            *number > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(*number);
}

std::vector<std::string> string_array_member(const cch::support::JsonValue& value, std::string_view name) {
    std::vector<std::string> members;
    const auto* object = value.get_if<cch::support::JsonValue::object_t>();
    if (object == nullptr) {
        return members;
    }
    const auto found = object->find(std::string{name});
    if (found == object->end()) {
        return members;
    }
    const auto* array = found->second.get_if<cch::support::JsonValue::array_t>();
    if (array == nullptr) {
        return members;
    }
    for (const auto& element : *array) {
        if (const auto* text = element.get_if<std::string>(); text != nullptr) {
            members.push_back(*text);
        }
    }
    return members;
}

std::vector<std::string> split_scopes(std::string_view scope) {
    std::vector<std::string> scopes;
    std::size_t start = 0;
    while (start <= scope.size()) {
        const auto separator = scope.find(' ', start);
        const auto end = separator == std::string_view::npos ? scope.size() : separator;
        if (end > start) {
            scopes.emplace_back(scope.substr(start, end - start));
        }
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1;
    }
    return scopes;
}

std::string error_of(const cch::support::JsonValue& value) {
    if (auto error = string_member(value, "error"); error.has_value() && !error->empty()) {
        return *error;
    }
    if (auto description = string_member(value, "error_description");
            description.has_value() && !description->empty()) {
        return *description;
    }
    return {};
}

} // namespace cch::mcp::oauth
