#include "mcp/WwwAuthenticate.hpp"

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::mcp::www_authenticate {
namespace {

/// The RFC 9110 `tchar` set. A challenge, a parameter name, and a `token68`
/// credential are runs of these, so every other byte — including `,`, `=`,
/// `"` and space — is structure the parser is allowed to act on.
[[nodiscard]] bool is_token_char(unsigned char byte) noexcept {
    if (std::isalnum(byte) != 0) {
        return true;
    }
    return std::string_view{"!#$%&'*+-.^_`|~"}.find(static_cast<char>(byte)) != std::string_view::npos;
}

/// The `OWS` the grammar puts around `,` and `=`.
[[nodiscard]] bool is_ows(unsigned char byte) noexcept { return byte == ' ' || byte == '\t'; }

/// A cursor over the field value. Every read is bounds-checked, so a truncated
/// or hostile header ends the parse rather than reading past it.
class Scanner {
public:
    explicit Scanner(std::string_view text) noexcept : text_(text) {}

    [[nodiscard]] std::size_t mark() const noexcept { return position_; }

    void restore(std::size_t position) noexcept { position_ = position; }

    [[nodiscard]] bool at_end() const noexcept { return position_ >= text_.size(); }

    [[nodiscard]] unsigned char peek() const noexcept {
        return at_end() ? 0U : static_cast<unsigned char>(text_[position_]);
    }

    void advance() noexcept { ++position_; }

    void skip_ows() noexcept {
        while (!at_end() && is_ows(peek())) {
            ++position_;
        }
    }

    /// Skip the separators between challenges: optional whitespace and any
    /// number of commas.
    void skip_separators() noexcept {
        skip_ows();
        while (!at_end() && peek() == ',') {
            advance();
            skip_ows();
        }
    }

    /// Read a `token` run. Empty when the cursor is not on a token byte, and
    /// consumed to the end of the run otherwise.
    [[nodiscard]] std::string read_token() noexcept {
        const auto start = position_;
        while (!at_end() && is_token_char(peek())) {
            ++position_;
        }
        return std::string{text_.substr(start, position_ - start)};
    }

    /// Read a `quoted-string`, resolving `\"` and `\\` escapes. A string that
    /// is never closed is malformed and reads as no value at all.
    [[nodiscard]] std::optional<std::string> read_quoted_string() noexcept {
        if (at_end() || peek() != '"') {
            return std::nullopt;
        }
        advance();
        std::string value;
        while (!at_end()) {
            const auto byte = peek();
            if (byte == '"') {
                advance();
                return value;
            }
            if (byte == '\\') {
                advance();
                if (at_end()) {
                    return std::nullopt;
                }
                value.push_back(static_cast<char>(peek()));
                advance();
                continue;
            }
            value.push_back(static_cast<char>(byte));
            advance();
        }
        return std::nullopt; // unterminated quoted-string
    }

    /// Read a parameter value: a `token` or a `quoted-string`. A value that
    /// starts with neither is malformed.
    [[nodiscard]] std::optional<std::string> read_param_value() noexcept {
        if (!at_end() && peek() == '"') {
            return read_quoted_string();
        }
        auto value = read_token();
        if (value.empty()) {
            return std::nullopt;
        }
        return value;
    }

private:
    std::string_view text_;
    std::size_t position_{0};
};

[[nodiscard]] std::string to_lower(std::string_view text) {
    std::string lowered{text};
    for (auto& byte : lowered) {
        byte = static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
    }
    return lowered;
}

[[nodiscard]] bool same_scheme(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto lhs = static_cast<unsigned char>(left[index]);
        const auto rhs = static_cast<unsigned char>(right[index]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }
    return true;
}

/// The first occurrence of a name wins, which is what a repeated `auth-param`
/// means: the field declares a requirement once and repeats it.
[[nodiscard]] const std::string* find_param(const RawChallenge& challenge, std::string_view name) noexcept {
    for (const auto& [param_name, value] : challenge.params) {
        if (param_name == name) {
            return &value;
        }
    }
    return nullptr;
}

/// Read one `auth-param` whose name the caller already consumed and whose `=`
/// the cursor is on, and record it under its lowercased name.
[[nodiscard]] bool read_param(RawChallenge& challenge, std::string name, Scanner& scanner) {
    scanner.advance(); // the '='
    scanner.skip_ows();
    auto value = scanner.read_param_value();
    if (!value.has_value()) {
        return false;
    }
    challenge.params.emplace_back(to_lower(name), std::move(*value));
    scanner.skip_ows();
    return true;
}

} // namespace

std::vector<RawChallenge> parse(std::string_view header) {
    std::vector<RawChallenge> challenges;
    Scanner scanner{header};
    while (true) {
        scanner.skip_separators();
        if (scanner.at_end()) {
            return challenges;
        }
        auto scheme = scanner.read_token();
        if (scheme.empty()) {
            // A byte that can begin neither a challenge nor a parameter.
            return challenges;
        }
        RawChallenge challenge{.scheme = std::move(scheme)};
        scanner.skip_ows();

        bool malformed = false;
        while (!scanner.at_end()) {
            if (scanner.peek() == ',') {
                // A comma ends this challenge's parameters or separates the
                // next challenge from them, and the grammar cannot say which.
                // It is a parameter when the run after the comma is a name
                // followed by `=`; otherwise the next challenge starts here.
                const auto comma = scanner.mark();
                scanner.advance();
                scanner.skip_ows();
                const auto next = scanner.read_token();
                scanner.skip_ows();
                if (next.empty() || scanner.at_end() || scanner.peek() != '=') {
                    scanner.restore(comma);
                    break;
                }
                if (!read_param(challenge, next, scanner)) {
                    malformed = true;
                    break;
                }
                continue;
            }
            auto name = scanner.read_token();
            if (name.empty()) {
                malformed = true;
                break;
            }
            scanner.skip_ows();
            if (scanner.at_end()) {
                // A scheme followed by one bare run is a `token68`
                // credential, not a parameter without a value.
                challenge.token68 = std::move(name);
                break;
            }
            if (scanner.peek() == '=') {
                if (!read_param(challenge, std::move(name), scanner)) {
                    malformed = true;
                    break;
                }
                continue;
            }
            // A bare run followed by anything but a value: a `token68`
            // credential, after which only a comma may continue the field.
            challenge.token68 = std::move(name);
            scanner.skip_ows();
            if (!scanner.at_end() && scanner.peek() != ',') {
                malformed = true;
            }
            break;
        }
        if (malformed) {
            // The challenge is dropped whole and the rest of the field with
            // it: a half-read authorization requirement is not actionable, and
            // acting on part of one is how a second challenge hides inside the
            // first.
            return challenges;
        }
        challenge.valid = true;
        challenges.push_back(std::move(challenge));
    }
}

AuthorizationChallenge reduce(const RawChallenge& challenge) {
    AuthorizationChallenge reduced{.scheme = challenge.scheme};
    if (const auto* resource_metadata = find_param(challenge, "resource_metadata"); resource_metadata != nullptr) {
        reduced.resource_metadata_url = *resource_metadata;
    }
    if (const auto* scope = find_param(challenge, "scope"); scope != nullptr) {
        reduced.scope = *scope;
    }
    for (const auto& [name, value] : challenge.params) {
        if (name == "error" && !value.empty()) {
            reduced.error_codes.push_back(value);
        }
    }
    return reduced;
}

std::optional<AuthorizationChallenge> first_bearer_challenge(std::string_view header) {
    for (const auto& challenge : parse(header)) {
        if (challenge.valid && same_scheme(challenge.scheme, "Bearer")) {
            return reduce(challenge);
        }
    }
    return std::nullopt;
}

} // namespace cch::mcp::www_authenticate
