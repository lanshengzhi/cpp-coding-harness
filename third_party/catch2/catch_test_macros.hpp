#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace CatchLite {
struct TestCase {
    std::string name;
    std::string tags;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(std::string name, std::string tags, std::function<void()> body) {
        registry().push_back({std::move(name), std::move(tags), std::move(body)});
    }
};

class AssertionFailure : public std::runtime_error {
public:
    explicit AssertionFailure(const std::string& message) : std::runtime_error(message) {}
};

inline void require(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        std::ostringstream out;
        out << file << ':' << line << ": REQUIRE(" << expression << ") failed";
        throw AssertionFailure(out.str());
    }
}

inline void check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        std::ostringstream out;
        out << file << ':' << line << ": CHECK(" << expression << ") failed";
        throw AssertionFailure(out.str());
    }
}
} // namespace CatchLite

#define CATCHLITE_CONCAT_INNER(a, b) a##b
#define CATCHLITE_CONCAT(a, b) CATCHLITE_CONCAT_INNER(a, b)

#define TEST_CASE(name, tags) \
    static void CATCHLITE_CONCAT(catchlite_test_, __LINE__)(); \
    static ::CatchLite::Registrar CATCHLITE_CONCAT(catchlite_registrar_, __LINE__){name, tags, CATCHLITE_CONCAT(catchlite_test_, __LINE__)}; \
    static void CATCHLITE_CONCAT(catchlite_test_, __LINE__)()

#define REQUIRE(expr) ::CatchLite::require(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define CHECK(expr) ::CatchLite::check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define REQUIRE_FALSE(expr) ::CatchLite::require(!static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define CHECK_FALSE(expr) ::CatchLite::check(!static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define REQUIRE_NOTHROW(expr) \
    do { \
        try { (void)(expr); } catch (const std::exception& e) { \
            std::ostringstream catchlite_out; \
            catchlite_out << __FILE__ << ':' << __LINE__ << ": REQUIRE_NOTHROW(" << #expr << ") threw " << e.what(); \
            throw ::CatchLite::AssertionFailure(catchlite_out.str()); \
        } \
    } while (false)
