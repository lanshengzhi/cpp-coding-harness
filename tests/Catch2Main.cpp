#include <catch2/catch_session.hpp>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace {

using SetupResult = std::expected<void, std::string>;

template <typename T>
using SetupValue = std::expected<T, std::string>;

class TestEnvironmentIsolation {
public:
    TestEnvironmentIsolation(TestEnvironmentIsolation&& other) noexcept
        : root_(std::move(other.root_)) {
        other.root_.clear();
    }

    TestEnvironmentIsolation& operator=(TestEnvironmentIsolation&& other) noexcept {
        if (this != &other) {
            remove_root();
            root_ = std::move(other.root_);
            other.root_.clear();
        }
        return *this;
    }

    ~TestEnvironmentIsolation() {
        remove_root();
    }

    TestEnvironmentIsolation(const TestEnvironmentIsolation&) = delete;
    TestEnvironmentIsolation& operator=(const TestEnvironmentIsolation&) = delete;

    static SetupValue<TestEnvironmentIsolation> create() {
        TestEnvironmentIsolation isolation;
        auto root = create_unique_root();
        if (!root) {
            return std::unexpected(std::move(root.error()));
        }
        isolation.root_ = std::move(*root);

        const auto home = isolation.create_directory("home");
        const auto agent_config = isolation.create_directory("agent");
        const auto temporary = isolation.create_directory("tmp");
        const auto xdg_config = isolation.create_directory("xdg-config");
        const auto xdg_cache = isolation.create_directory("xdg-cache");
        const auto xdg_data = isolation.create_directory("xdg-data");
        if (!home || !agent_config || !temporary || !xdg_config || !xdg_cache || !xdg_data) {
            return std::unexpected("could not create all test isolation directories");
        }

        const std::pair<const char*, const std::filesystem::path*> environment[] = {
            {"HOME", &*home},
            {"PI_CODING_AGENT_DIR", &*agent_config},
            {"TMPDIR", &*temporary},
            {"XDG_CONFIG_HOME", &*xdg_config},
            {"XDG_CACHE_HOME", &*xdg_cache},
            {"XDG_DATA_HOME", &*xdg_data},
        };
        for (const auto& [name, value] : environment) {
            if (auto result = set_environment(name, *value); !result) {
                return std::unexpected(std::move(result.error()));
            }
        }
        if (auto result = unset_environment("PI_CODING_AGENT_SESSION_DIR"); !result) {
            return std::unexpected(std::move(result.error()));
        }
        return isolation;
    }

private:
    TestEnvironmentIsolation() = default;

    static SetupValue<std::filesystem::path> create_unique_root() {
        const std::filesystem::path base{"/tmp"};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        std::error_code error;
        for (std::size_t attempt = 0; attempt < 100; ++attempt) {
            const auto candidate = base / ("cpp-harness-catch2-" + std::to_string(::getpid()) + "-" +
                                           std::to_string(stamp) + "-" + std::to_string(attempt));
            error.clear();
            if (std::filesystem::create_directory(candidate, error)) {
                return candidate;
            }
            if (error && error != std::errc::file_exists) {
                return std::unexpected("could not create the test isolation directory: " + error.message());
            }
        }
        return std::unexpected("could not allocate a unique test isolation directory");
    }

    SetupValue<std::filesystem::path> create_directory(const std::string& name) const {
        const auto path = root_ / name;
        std::error_code error;
        if (!std::filesystem::create_directory(path, error)) {
            return std::unexpected("could not create isolated " + name + " directory: " + error.message());
        }
        return path;
    }

    static SetupResult set_environment(const char* name, const std::filesystem::path& value) {
        if (::setenv(name, value.c_str(), 1) != 0) {
            return std::unexpected("could not isolate environment variable " + std::string{name});
        }
        return {};
    }

    static SetupResult unset_environment(const char* name) {
        if (::unsetenv(name) != 0) {
            return std::unexpected("could not clear environment variable " + std::string{name});
        }
        return {};
    }

    void remove_root() noexcept {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    std::filesystem::path root_;
};

} // namespace

int main(int argc, char** argv) {
    auto isolation = TestEnvironmentIsolation::create();
    if (!isolation) {
        std::cerr << "Catch2 test environment setup failed: " << isolation.error() << '\n';
        return 1;
    }
    return Catch::Session().run(argc, argv);
}
