#pragma once

#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

namespace cch::tests {

/// Save and restore an environment variable around a test. Constructing with
/// only a name saves the current value for later set()/unset() calls;
/// constructing with an override value applies it immediately (unset when
/// nullopt). The previous state is always restored on destruction.
class EnvVarGuard final {
public:
    explicit EnvVarGuard(std::string name) : name_(std::move(name)) {
        save();
    }

    EnvVarGuard(std::string name, std::optional<std::string> override_value)
        : name_(std::move(name)) {
        save();
        if (override_value) {
            set(*override_value);
        } else {
            unset();
        }
    }

    ~EnvVarGuard() {
        if (previous_) {
            setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

    EnvVarGuard(const EnvVarGuard&) = delete;
    EnvVarGuard& operator=(const EnvVarGuard&) = delete;

    void set(const std::string& value) const {
        setenv(name_.c_str(), value.c_str(), 1);
    }

    void unset() const {
        unsetenv(name_.c_str());
    }

private:
    void save() {
        if (const char* value = std::getenv(name_.c_str()); value != nullptr) {
            previous_ = value;
        }
    }

    std::string name_;
    std::optional<std::string> previous_;
};

} // namespace cch::tests
