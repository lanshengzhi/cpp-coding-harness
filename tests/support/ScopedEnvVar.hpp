#pragma once

#include <cstdlib>
#include <optional>
#include <string>

namespace cch::tests {

/// RAII process-environment override for tests that exercise
/// environment-dependent resolution (e.g. "~" expanding against $HOME). Sets
/// the variable on construction and restores the prior state — including
/// "unset" — on destruction, so a failing REQUIRE cannot leak the override
/// into the rest of the process.
class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const std::string& value) : name_(name) {
        if (const char* previous = std::getenv(name); previous != nullptr) {
            saved_ = previous;
        }
        ok_ = ::setenv(name, value.c_str(), 1) == 0;
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

    ~ScopedEnvVar() {
        if (saved_) {
            (void)::setenv(name_.c_str(), saved_->c_str(), 1);
        } else {
            (void)::unsetenv(name_.c_str());
        }
    }

    /// False when the override could not be installed; tests REQUIRE this
    /// before exercising environment-dependent behavior.
    [[nodiscard]] bool ok() const { return ok_; }

private:
    std::string name_;
    std::optional<std::string> saved_;
    bool ok_{false};
};

} // namespace cch::tests
