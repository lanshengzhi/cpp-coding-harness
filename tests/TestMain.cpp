#include "../third_party/catch2/catch_session.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

/// Isolate every test from the developer's real user state (`~/.pi/agent`,
/// `~/.pi/settings.json`, `~/.pi/agent/trust.json`). Without this, in-process
/// SessionFactory creation and subprocess CLI runs would read the live
/// `settings.json`/`models.json` (e.g. `defaultProvider`), making the suite
/// depend on the host environment. Individual tests that set `HOME`/
/// `PI_CODING_AGENT_DIR` via EnvVarGuard override and restore this.
class TestHomeIsolation {
public:
    TestHomeIsolation() {
        const auto base = std::filesystem::temp_directory_path();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto home = base / ("cpp-harness-test-home-" + std::to_string(stamp));
        std::error_code ec;
        std::filesystem::create_directories(home, ec);
        path_ = home.string();
#if defined(_WIN32)
        (void)::_putenv_s("HOME", path_.c_str());
#else
        setenv("HOME", path_.c_str(), 1);
#endif
    }

    ~TestHomeIsolation() {
#if defined(_WIN32)
        (void)::_putenv_s("HOME", "");
#else
        unsetenv("HOME");
#endif
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TestHomeIsolation(const TestHomeIsolation&) = delete;
    TestHomeIsolation& operator=(const TestHomeIsolation&) = delete;

private:
    std::string path_;
};

} // namespace

int main(int argc, char** argv) {
    TestHomeIsolation isolation;
    return Catch::Session().run(argc, argv);
}
