#include "InMemorySessionStore.hpp"

namespace cch::harness::session {

util::ExpectedVoid InMemorySessionStore::append(const ai::MessageVariant& /*message*/) {
    return {};
}

std::optional<std::filesystem::path> InMemorySessionStore::path() const {
    return std::nullopt;
}

} // namespace cch::harness::session
