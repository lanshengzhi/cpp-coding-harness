#pragma once

#include <cstddef>
#include <random>
#include <string>

namespace cch::support {

/// Generate `count` lowercase hexadecimal characters using a thread-local PRNG.
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

} // namespace cch::support
