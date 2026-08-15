#include "Pkce.hpp"

#include "support/Json.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace cch::ai::auth {
namespace {

constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] support::Expected<std::string> random_bytes(std::size_t count) {
    std::string bytes(count, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(bytes.data()),
                   static_cast<int>(count)) != 1) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "secure random bytes unavailable"));
    }
    return bytes;
}

[[nodiscard]] support::Expected<std::string> sha256(std::string_view data) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_length = 0;
    if (EVP_Digest(
            data.data(),
            data.size(),
            digest.data(),
            &digest_length,
            EVP_sha256(),
            nullptr) != 1) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "SHA-256 digest failed"));
    }
    return std::string(
        reinterpret_cast<const char*>(digest.data()),
        digest_length);
}

[[nodiscard]] int base64_value(char character) {
    const auto index = kBase64Alphabet.find(character);
    return index == std::string_view::npos ? -1 : static_cast<int>(index);
}

[[nodiscard]] std::string base64_encode(std::string_view bytes) {
    std::string result;
    result.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t index = 0;
    while (index + 3 <= bytes.size()) {
        const auto first = static_cast<unsigned char>(bytes[index]);
        const auto second = static_cast<unsigned char>(bytes[index + 1]);
        const auto third = static_cast<unsigned char>(bytes[index + 2]);
        result.push_back(kBase64Alphabet[(first >> 2) & 0x3F]);
        result.push_back(kBase64Alphabet[((first << 4) | (second >> 4)) & 0x3F]);
        result.push_back(kBase64Alphabet[((second << 2) | (third >> 6)) & 0x3F]);
        result.push_back(kBase64Alphabet[third & 0x3F]);
        index += 3;
    }
    const auto remaining = bytes.size() - index;
    if (remaining == 1) {
        const auto first = static_cast<unsigned char>(bytes[index]);
        result.push_back(kBase64Alphabet[(first >> 2) & 0x3F]);
        result.push_back(kBase64Alphabet[(first << 4) & 0x3F]);
        result.push_back('=');
        result.push_back('=');
    } else if (remaining == 2) {
        const auto first = static_cast<unsigned char>(bytes[index]);
        const auto second = static_cast<unsigned char>(bytes[index + 1]);
        result.push_back(kBase64Alphabet[(first >> 2) & 0x3F]);
        result.push_back(kBase64Alphabet[((first << 4) | (second >> 4)) & 0x3F]);
        result.push_back(kBase64Alphabet[(second << 2) & 0x3F]);
        result.push_back('=');
    }
    return result;
}

[[nodiscard]] support::Expected<std::string> base64_decode(std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (const char character : text) {
        if (character == '-') {
            normalized.push_back('+');
        } else if (character == '_') {
            normalized.push_back('/');
        } else if (character != '=') {
            normalized.push_back(character);
        }
    }
    // atob is forgiving about missing padding; pad to a multiple of four.
    const auto remainder = normalized.size() % 4;
    if (remainder == 1) {
        return std::unexpected(support::make_error(
            support::ErrorCode::JsonParse,
            "invalid base64 length"));
    }
    if (remainder != 0) {
        normalized.append(4 - remainder, '=');
    }

    std::string result;
    result.reserve(normalized.size() / 4 * 3);
    for (std::size_t index = 0; index < normalized.size(); index += 4) {
        const int first = base64_value(normalized[index]);
        const int second = base64_value(normalized[index + 1]);
        const int third = base64_value(normalized[index + 2]);
        const int fourth = base64_value(normalized[index + 3]);
        if (first < 0 || second < 0 ||
            (normalized[index + 2] != '=' && third < 0) ||
            (normalized[index + 3] != '=' && fourth < 0)) {
            return std::unexpected(support::make_error(
                support::ErrorCode::JsonParse,
                "invalid base64 character"));
        }
        result.push_back(static_cast<char>((first << 2) | (second >> 4)));
        if (normalized[index + 2] != '=') {
            result.push_back(static_cast<char>(
                ((second << 4) & 0xF0) | (third >> 2)));
        }
        if (normalized[index + 3] != '=') {
            result.push_back(static_cast<char>(
                ((third << 6) & 0xC0) | fourth));
        }
    }
    return result;
}

[[nodiscard]] support::Expected<support::JsonValue> decode_jwt_payload(
    std::string_view token) {
    const std::size_t first_dot = token.find('.');
    if (first_dot == std::string_view::npos) {
        return std::unexpected(support::make_error(
            support::ErrorCode::JsonParse,
            "JWT has no payload segment"));
    }
    const std::size_t second_dot = token.find('.', first_dot + 1);
    if (second_dot == std::string_view::npos ||
        token.find('.', second_dot + 1) != std::string_view::npos) {
        return std::unexpected(support::make_error(
            support::ErrorCode::JsonParse,
            "JWT is not a three-part token"));
    }
    const auto payload = token.substr(first_dot + 1, second_dot - first_dot - 1);
    if (auto decoded = base64_decode(payload); !decoded) {
        return std::unexpected(std::move(decoded.error()));
    } else {
        if (auto json = support::read_json(*decoded); !json) {
            return std::unexpected(std::move(json.error()));
        } else {
            return std::move(*json);
        }
    }
}

} // namespace

std::string base64url_encode(std::string_view bytes) {
    auto encoded = base64_encode(bytes);
    for (auto& character : encoded) {
        if (character == '+') {
            character = '-';
        } else if (character == '/') {
            character = '_';
        }
    }
    std::erase(encoded, '=');
    return encoded;
}

support::Expected<PkcePair> generate_pkce() {
    if (auto verifier_bytes = random_bytes(32); !verifier_bytes) {
        return std::unexpected(std::move(verifier_bytes.error()));
    } else {
        const std::string verifier = base64url_encode(*verifier_bytes);
        if (auto hashed = sha256(verifier); !hashed) {
            return std::unexpected(std::move(hashed.error()));
        } else {
            const std::string challenge = base64url_encode(*hashed);
            return PkcePair{
                .verifier = verifier,
                .challenge = challenge,
            };
        }
    }
}

support::Expected<std::string> create_oauth_state() {
    if (auto bytes = random_bytes(16); !bytes) {
        return std::unexpected(std::move(bytes.error()));
    } else {
        std::string state;
        state.reserve(32);
        for (const unsigned char byte : *bytes) {
            const auto push_hex = [&state](unsigned char nibble) {
                state.push_back(nibble < 10
                    ? static_cast<char>('0' + nibble)
                    : static_cast<char>('a' + nibble - 10));
            };
            push_hex(byte >> 4);
            push_hex(byte & 0x0F);
        }
        return state;
    }
}

std::string url_query_encode(std::string_view value) {
    constexpr std::string_view kHex = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size());
    for (const unsigned char byte : value) {
        const auto is_unreserved = [](unsigned char character) {
            return (character >= 'A' && character <= 'Z') ||
                   (character >= 'a' && character <= 'z') ||
                   (character >= '0' && character <= '9') ||
                   character == '*' || character == '-' ||
                   character == '.' || character == '_';
        };
        if (is_unreserved(byte)) {
            result.push_back(static_cast<char>(byte));
        } else if (byte == ' ') {
            result.push_back('+');
        } else {
            result.push_back('%');
            result.push_back(kHex[byte >> 4]);
            result.push_back(kHex[byte & 0x0F]);
        }
    }
    return result;
}

std::string url_query_decode(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    const auto hex_value = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        if (character == '+') {
            result.push_back(' ');
        } else if (character == '%' && index + 2 < value.size()) {
            const int high = hex_value(value[index + 1]);
            const int low = hex_value(value[index + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                index += 2;
            } else {
                result.push_back(character);
            }
        } else {
            result.push_back(character);
        }
    }
    return result;
}

std::map<std::string, std::string, std::less<>> parse_query_pairs(
    std::string_view query) {
    std::map<std::string, std::string, std::less<>> pairs;
    std::size_t offset = 0;
    while (offset <= query.size()) {
        const auto ampersand = query.find('&', offset);
        const auto pair = query.substr(
            offset,
            (ampersand == std::string_view::npos ? query.size() : ampersand) -
                offset);
        const auto equals = pair.find('=');
        const auto key = url_query_decode(pair.substr(0, equals));
        const auto value = equals == std::string_view::npos
            ? std::string{}
            : url_query_decode(pair.substr(equals + 1));
        // First value wins, matching URLSearchParams.get.
        pairs.emplace(key, value);
        if (ampersand == std::string_view::npos) {
            break;
        }
        offset = ampersand + 1;
    }
    return pairs;
}

AuthorizationInput parse_authorization_input(std::string_view input) {
    const std::string value{input};
    const auto trimmed_start = value.find_first_not_of(" \t\r\n");
    const auto trimmed = trimmed_start == std::string::npos
        ? std::string{}
        : value.substr(
              trimmed_start,
              value.find_last_not_of(" \t\r\n") - trimmed_start + 1);
    if (trimmed.empty()) {
        return {};
    }

    // Full redirect URL: pi `new URL(value)` succeeds and query params win.
    const auto scheme_end = trimmed.find("://");
    if (scheme_end != std::string::npos) {
        const auto scheme = trimmed.substr(0, scheme_end);
        const auto valid_scheme = !scheme.empty() &&
            ((scheme[0] >= 'A' && scheme[0] <= 'Z') ||
             (scheme[0] >= 'a' && scheme[0] <= 'z')) &&
            std::all_of(scheme.begin() + 1, scheme.end(), [](char character) {
                return (character >= 'A' && character <= 'Z') ||
                       (character >= 'a' && character <= 'z') ||
                       (character >= '0' && character <= '9') ||
                       character == '+' || character == '.' || character == '-';
            });
        if (valid_scheme) {
            const auto query_start = trimmed.find('?');
            if (query_start == std::string::npos) {
                return {};
            }
            const auto query_end = trimmed.find('#', query_start);
            const auto query = trimmed.substr(
                query_start + 1,
                (query_end == std::string::npos ? trimmed.size() : query_end) -
                    query_start - 1);
            const auto pairs = parse_query_pairs(query);
            AuthorizationInput parsed;
            if (const auto found = pairs.find("code"); found != pairs.end()) {
                parsed.code = found->second;
            }
            if (const auto found = pairs.find("state"); found != pairs.end()) {
                parsed.state = found->second;
            }
            return parsed;
        }
    }

    if (trimmed.find('#') != std::string::npos) {
        const auto hash = trimmed.find('#');
        return AuthorizationInput{
            .code = trimmed.substr(0, hash),
            .state = trimmed.substr(hash + 1),
        };
    }

    if (trimmed.find("code=") != std::string::npos) {
        const auto pairs = parse_query_pairs(trimmed);
        AuthorizationInput parsed;
        if (const auto found = pairs.find("code"); found != pairs.end()) {
            parsed.code = found->second;
        }
        if (const auto found = pairs.find("state"); found != pairs.end()) {
            parsed.state = found->second;
        }
        return parsed;
    }

    return AuthorizationInput{.code = trimmed};
}

support::Expected<std::string> extract_account_id(std::string_view access_token) {
    if (auto payload = decode_jwt_payload(access_token); !payload) {
        return std::unexpected(std::move(payload.error()));
    } else {
        constexpr std::string_view kClaimPath = "https://api.openai.com/auth";
        const auto* claim = payload->get_if<support::JsonValue::object_t>();
        if (claim == nullptr) {
            return std::unexpected(support::make_error(
                support::ErrorCode::JsonParse,
                "JWT payload is not an object"));
        }
        const auto claim_found = claim->find(std::string{kClaimPath});
        if (claim_found == claim->end()) {
            return std::unexpected(support::make_error(
                support::ErrorCode::JsonParse,
                "JWT payload has no auth claim"));
        }
        const auto* auth = claim_found->second.get_if<support::JsonValue::object_t>();
        if (auth == nullptr) {
            return std::unexpected(support::make_error(
                support::ErrorCode::JsonParse,
                "JWT auth claim is not an object"));
        }
        const auto account_found = auth->find("chatgpt_account_id");
        if (account_found == auth->end()) {
            return std::unexpected(support::make_error(
                support::ErrorCode::JsonParse,
                "JWT auth claim has no chatgpt_account_id"));
        }
        const auto* account_id = account_found->second.get_if<std::string>();
        if (account_id == nullptr || account_id->empty()) {
            return std::unexpected(support::make_error(
                support::ErrorCode::JsonParse,
                "JWT chatgpt_account_id is not a non-empty string"));
        }
        return *account_id;
    }
}

} // namespace cch::ai::auth
