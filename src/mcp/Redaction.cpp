#include "mcp/Redaction.hpp"

#include <cch/support/BoundedText.hpp>
#include "mcp/Protocol.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace cch::mcp::redaction {
namespace {

/// The shortest value the credential erasure will remove from arbitrary text.
/// A shorter value matches too much of the surrounding text to be erased
/// without destroying the diagnostic it appears in, so it is left to the
/// shape-based rules, which still cover any form that carries a key.
constexpr std::size_t kMinErasableSecretLength{8};

/// Whether a value is distinctive enough to erase from arbitrary text.
[[nodiscard]] bool erasable(std::string_view secret) {
    return secret.size() >= kMinErasableSecretLength;
}

[[nodiscard]] std::string erase_all(std::string text, std::string_view secret) {
    if (!erasable(secret)) {
        return text;
    }
    std::size_t position = 0;
    while ((position = text.find(secret, position)) != std::string::npos) {
        text.replace(position, secret.size(), cch::support::kRedactionMarker);
        position += cch::support::kRedactionMarker.size();
    }
    return text;
}

} // namespace

std::string bounded_diagnostic(std::string text) {
    return cch::support::bounded_redacted_text(std::move(text), protocol::kMaxDiagnosticBytes, "...");
}

std::string erase_credential(std::string text, std::string_view secret) {
    return erase_all(std::move(text), secret);
}

std::string redacted_text(std::string text, std::string_view secret) {
    // The credential is erased before the shared rules and before the bound
    // see the text, so neither a shape rule nor a truncation point can leave
    // any part of it behind.
    return bounded_diagnostic(erase_credential(std::move(text), secret));
}

cch::support::JsonValue redacted_value(cch::support::JsonValue value, std::string_view secret) {
    if (!erasable(secret)) {
        return value;
    }
    auto* const text = value.get_if<std::string>();
    if (text != nullptr) {
        *text = erase_credential(std::move(*text), secret);
        return value;
    }
    if (auto* const items = value.get_if<cch::support::JsonValue::array_t>(); items != nullptr) {
        for (auto& item : *items) {
            item = redacted_value(std::move(item), secret);
        }
        return value;
    }
    if (auto* const members = value.get_if<cch::support::JsonValue::object_t>(); members != nullptr) {
        for (auto& [name, member] : *members) {
            member = redacted_value(std::move(member), secret);
        }
        return value;
    }
    return value;
}

} // namespace cch::mcp::redaction
