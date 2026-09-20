#pragma once

#include <random>
#include <string>

namespace cch::harness::session {

/// `count` lowercase hex characters from the thread-local generator: the
/// pi-shaped isolation ids (compaction's fresh summarization session id
/// takes 32, entry ids 8; pi `uuidv7()` shapes the purpose, not the format).
[[nodiscard]] inline std::string random_hex_id(std::size_t count) {
    thread_local std::random_device rd;
    thread_local std::mt19937_64 gen(rd());
    thread_local std::uniform_int_distribution<unsigned> dist(0, 15);
    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string id(count, '0');
    for (auto& character : id) {
        character = hex_chars[dist(gen)];
    }
    return id;
}

} // namespace cch::harness::session
