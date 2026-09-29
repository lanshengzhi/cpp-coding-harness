#include <cch/coding_agent/McpToolBinding.hpp>

#include <array>
#include <cstdint>
#include <format>

namespace cch::coding_agent {
namespace {

/// Every character outside `[a-zA-Z0-9_-]` becomes this one. Sanitizing is
/// lossy on purpose: a tool name is a third party's string, and the registered
/// name has to be safe for every provider's tool-name grammar.
constexpr char kSanitizedCharacter{'_'};

[[nodiscard]] constexpr char sanitize_character(char character) noexcept {
    const bool alphanumeric = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                              (character >= '0' && character <= '9');
    return (alphanumeric || character == '_' || character == '-') ? character : kSanitizedCharacter;
}

[[nodiscard]] std::string sanitize(std::string_view text) {
    std::string sanitized;
    sanitized.reserve(text.size());
    for (const char character : text) {
        sanitized.push_back(sanitize_character(character));
    }
    return sanitized;
}

/// FNV-1a over the sanitized name. Deterministic across runs and platforms, so
/// the same long tool name registers under the same truncated name in every
/// session that discovers it.
[[nodiscard]] std::string hash_suffix(std::string_view sanitized) {
    std::uint64_t hash{14695981039346656037ULL};
    for (const char character : sanitized) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(character));
        hash *= 1099511628211ULL;
    }
    std::array<char, 8> digits{};
    static constexpr std::array<char, 16> kHexDigits{
            '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    for (std::size_t index = 0; index < digits.size(); ++index) {
        const auto nibble = static_cast<std::size_t>((hash >> (index * 4)) & 0xfU);
        digits[digits.size() - 1 - index] = kHexDigits[nibble];
    }
    return std::string{digits.data(), digits.size()};
}

} // namespace

std::string mcp_qualified_tool_name(std::string_view server_id, std::string_view tool_name) {
    std::string qualified = "mcp__";
    qualified += sanitize(server_id);
    qualified += "__";
    qualified += sanitize(tool_name);
    if (qualified.size() <= kMcpQualifiedToolNameMaxLength) {
        return qualified;
    }
    // Keep the readable head, then `_<hash>`: the suffix is what makes the
    // truncation injective enough that two long names do not collide, and the
    // head is what keeps the name debuggable in a transcript.
    const std::string suffix = hash_suffix(qualified);
    constexpr std::size_t kSeparator{1};
    const std::size_t keep = kMcpQualifiedToolNameMaxLength - kSeparator - suffix.size();
    qualified.resize(keep);
    qualified += '_';
    qualified += suffix;
    return qualified;
}

} // namespace cch::coding_agent
