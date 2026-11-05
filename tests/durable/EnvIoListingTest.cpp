#include <cch/durable/Environment.hpp>

#include <cch/support/JsonValue.hpp>

#include "durable/FrozenExpectation.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using cch::support::JsonValue;
namespace fs = std::filesystem;

namespace {

/// Restores one environment variable at scope end; tests own the
/// environment state they change (CODING_STANDARDS.md §11.7).
class EnvGuard {
public:
    explicit EnvGuard(std::string name) : name_(std::move(name)) {
        if (const char* value = std::getenv(name_.c_str())) {
            had_value_ = true;
            old_value_ = value;
        }
    }
    ~EnvGuard() {
        if (had_value_)
            ::setenv(name_.c_str(), old_value_.c_str(), 1);
        else
            ::unsetenv(name_.c_str());
    }
    void set(const std::string& value) { ::setenv(name_.c_str(), value.c_str(), 1); }
    void unset() { ::unsetenv(name_.c_str()); }

private:
    std::string name_;
    bool had_value_{};
    std::string old_value_{};
};

/// The neutral environment the captures ran with: nothing host-specific
/// leaks in through HOME, XDG or the git config variables.
void apply_neutral_env(const fs::path& scratch) {
    static EnvGuard home("HOME");
    static EnvGuard xdg("XDG_CONFIG_HOME");
    static EnvGuard global("GIT_CONFIG_GLOBAL");
    static EnvGuard system("GIT_CONFIG_SYSTEM");
    static EnvGuard git_dir("GIT_DIR");
    static EnvGuard git_work_tree("GIT_WORK_TREE");
    fs::create_directories(scratch / "env-home");
    fs::create_directories(scratch / "env-xdg");
    home.set((scratch / "env-home").string());
    xdg.set((scratch / "env-xdg").string());
    global.set(scratch / "env-no-such-global");
    system.set(scratch / "env-no-such-system");
    git_dir.unset();
    git_work_tree.unset();
}

void write_tree(const fs::path& root, const JsonValue& files) {
    if (const auto* names = files.get_if<JsonValue::array_t>()) {
        for (const auto& item : *names)
            write_tree(root, item);
        return;
    }
    const auto* name = files.get_if<std::string>();
    if (name != nullptr) {
        // A raw-name marker is written by its owning scenario, not here.
        if (name->starts_with("<bytes ")) return;
        if (name->ends_with('/')) {
            fs::create_directories(root / *name);
        } else {
            fs::create_directories((root / *name).parent_path());
            std::ofstream output(root / *name, std::ios::binary | std::ios::trunc);
        }
        return;
    }
    const auto* object = files.get_if<JsonValue::object_t>();
    REQUIRE(object != nullptr);
    for (const auto& [relative, content] : *object) {
        if (relative.starts_with("<bytes ")) continue; // raw-name marker, written by its owning scenario
        const auto target = root / relative;
        if (relative.ends_with('/')) {
            fs::create_directories(target);
            continue;
        }
        fs::create_directories(target.parent_path());
        const auto* text = content.get_if<std::string>();
        std::ofstream output(target, std::ios::binary | std::ios::trunc);
        if (text != nullptr) output << *text;
    }
}

cch::durable::TreeListingOptions options_of(const JsonValue& scenario, const char* key = "options") {
    cch::durable::TreeListingOptions options;
    if (scenario.at("inputs").get_object().find(key) == scenario.at("inputs").get_object().cend()) return options;
    const auto& json = scenario.at("inputs").at(key);
    if (const auto* hidden = json.get_object().find("hidden") == json.get_object().cend()
                                     ? nullptr
                                     : json.at("hidden").get_if<bool>())
        options.hidden = *hidden;
    if (const auto* gitignore = json.get_object().find("gitignore") == json.get_object().cend()
                                        ? nullptr
                                        : json.at("gitignore").get_if<bool>())
        options.gitignore = *gitignore;
    if (const auto* entries = json.get_object().find("maxEntries") == json.get_object().cend()
                                      ? nullptr
                                      : json.at("maxEntries").get_if<double>())
        options.max_entries = static_cast<std::size_t>(*entries);
    if (const auto* bytes = json.get_object().find("maxBytes") == json.get_object().cend()
                                    ? nullptr
                                    : json.at("maxBytes").get_if<double>())
        options.max_bytes = static_cast<std::size_t>(*bytes);
    if (const auto* timeout = json.get_object().find("timeoutMs") == json.get_object().cend()
                                      ? nullptr
                                      : json.at("timeoutMs").get_if<double>())
        options.timeout = std::chrono::milliseconds(static_cast<long long>(*timeout));
    return options;
}

void check_listing(const cch::durable::TreeListing& actual, const JsonValue& expected) {
    const auto* entries = expected.at("entries").get_if<JsonValue::array_t>();
    REQUIRE(entries != nullptr);
    CHECK(actual.truncated == expected.at("truncated").get_boolean());
    std::vector<std::string> actual_paths;
    std::vector<char> actual_kinds;
    for (const auto& entry : actual.entries) {
        actual_paths.push_back(entry.path);
        actual_kinds.push_back(entry.kind);
    }
    std::vector<std::string> expected_paths;
    std::vector<char> expected_kinds;
    for (const auto& entry : *entries) {
        expected_paths.push_back(entry.at("path").get_string());
        const auto& kind = entry.at("kind").get_string();
        expected_kinds.push_back(kind == "directory" ? 'd' : kind == "symlink" ? 'l' : 'f');
    }
    CHECK(actual_paths == expected_paths);
    CHECK(actual_kinds == expected_kinds);
}

/// Drives one captured scenario end to end: rebuild the tree, list it, and
/// compare every entry path, kind and the truncated flag.
void run_scenario(const JsonValue& scenario,
        const fs::path& root,
        const cch::durable::TreeListingOptions& options,
        const std::string& list_path = ".") {
    write_tree(root, scenario.at("inputs").at("files"));
    auto env = cch::durable::local_environment(root.string());
    auto listing = env->list_files(list_path, options, {});
    INFO(std::format("scenario: {}", scenario.at("name").get_string()));
    if (!listing) WARN(std::format("ERROR: {} root={}", listing.error().message, root.string()));
    REQUIRE(listing.has_value());
    check_listing(*listing, scenario.at("expected"));
}

} // namespace

TEST_CASE("Durable tree listing matches the frozen ignore-rule observations", "[durable][environment][issue1198]") {
    const auto artifact = cch::tests::durable::frozen_artifact("fixtures/pi-v1.3.0/audit-1198/ignore-rules.json");
    const auto* scenarios = artifact.at("scenarios").get_if<JsonValue::array_t>();
    REQUIRE(scenarios != nullptr);

    std::size_t index = 0;
    for (const auto& scenario : *scenarios) {
        const auto name = scenario.at("name").get_string();
        cch::tests::TempWorkspace workspace;
        apply_neutral_env(workspace.path());
        const auto root = workspace.path() / "case";
        fs::create_directories(root);
        const auto options = options_of(scenario);

        if (name == "symlinks list as links and a fifo is skipped") {
            write_tree(root, scenario.at("inputs").at("files"));
            ::symlink("target.txt", (root / "link").c_str());
            ::symlink("missing", (root / "dangling").c_str());
            ::mkfifo((root / "pipe").c_str(), 0600);
            auto env = cch::durable::local_environment(root.string());
            auto listing = env->list_files(".", options, {});
            INFO(std::format("scenario: {}", name));
            REQUIRE(listing.has_value());
            check_listing(*listing, scenario.at("expected"));
        } else if (name == "a non-utf8 name lists with replacement characters") {
            write_tree(root, scenario.at("inputs").at("files"));
            const std::string raw_name = std::string("bad") + char(0xff) + ".txt";
            std::ofstream output(root / raw_name, std::ios::binary | std::ios::trunc);
            auto env = cch::durable::local_environment(root.string());
            auto listing = env->list_files(".", options, {});
            INFO(std::format("scenario: {}", name));
            REQUIRE(listing.has_value());
            check_listing(*listing, scenario.at("expected"));
        } else if (name == "zero exact and over bounds report truncated exactly") {
            write_tree(root, scenario.at("inputs").at("files"));
            const auto* cases = scenario.at("expected").at("cases").get_if<JsonValue::array_t>();
            REQUIRE(cases != nullptr);
            for (const auto& bound_case : *cases) {
                cch::durable::TreeListingOptions case_options;
                case_options.gitignore = false;
                if (const auto* entries = bound_case.at("options").get_object().find("maxEntries") ==
                                                          bound_case.at("options").get_object().cend()
                                                  ? nullptr
                                                  : bound_case.at("options").at("maxEntries").get_if<double>())
                    case_options.max_entries = static_cast<std::size_t>(*entries);
                if (const auto* bytes = bound_case.at("options").get_object().find("maxBytes") ==
                                                        bound_case.at("options").get_object().cend()
                                                ? nullptr
                                                : bound_case.at("options").at("maxBytes").get_if<double>())
                    case_options.max_bytes = static_cast<std::size_t>(*bytes);
                if (const auto* timeout = bound_case.at("options").get_object().find("timeoutMs") ==
                                                          bound_case.at("options").get_object().cend()
                                                  ? nullptr
                                                  : bound_case.at("options").at("timeoutMs").get_if<double>())
                    case_options.timeout = std::chrono::milliseconds(static_cast<long long>(*timeout));
                auto env = cch::durable::local_environment(root.string());
                auto listing = env->list_files(".", case_options, {});
                const std::string case_label = [&] {
                    std::string text;
                    for (const auto& [key, value] : bound_case.at("options").get_object())
                        text += key + "=" + std::to_string(value.get_number()) + " ";
                    return text;
                }();
                INFO(std::format("scenario: {} case {}", name, case_label));
                REQUIRE(listing.has_value());
                check_listing(*listing, bound_case.at("listing"));
            }
            (void)options;
        } else if (name == "gitignore false disables the rules while hidden filtering stays") {
            write_tree(root, scenario.at("inputs").at("files"));
            auto env = cch::durable::local_environment(root.string());
            INFO(std::format("scenario: {}", name));
            auto off = env->list_files(".", options_of(scenario, "optionsOff"), {});
            REQUIRE(off.has_value());
            check_listing(*off, scenario.at("expected").at("off"));
            auto on = env->list_files(".", options_of(scenario, "optionsOn"), {});
            REQUIRE(on.has_value());
            check_listing(*on, scenario.at("expected").at("on"));
        } else if (name == "gitignore beats info-exclude and the global file") {
            // The capture pointed $XDG_CONFIG_HOME at a fixture whose default
            // ignore file (git/ignore) holds the global rules.
            write_tree(root, scenario.at("inputs").at("files"));
            const auto xdg = workspace.path() / "prec-xdg";
            fs::create_directories(xdg / "git");
            std::ofstream(xdg / "git/ignore") << "from-global\nfrom-global-extra\n";
            static EnvGuard xdg_env("XDG_CONFIG_HOME");
            xdg_env.set(xdg.string());
            auto env = cch::durable::local_environment(root.string());
            auto listing = env->list_files(".", options, {});
            INFO(std::format("scenario: {}", name));
            REQUIRE(listing.has_value());
            check_listing(*listing, scenario.at("expected"));
        } else {
            run_scenario(scenario, root, options);
        }
        ++index;
    }
    CHECK(index == 26);
}

TEST_CASE("Durable tree listing matches the frozen discovery observations", "[durable][environment][issue1199]") {
    const auto artifact = cch::tests::durable::frozen_artifact("fixtures/pi-v1.3.0/audit-1199/discovery.json");
    const auto* scenarios = artifact.at("scenarios").get_if<JsonValue::array_t>();
    REQUIRE(scenarios != nullptr);

    std::size_t index = 0;
    for (const auto& scenario : *scenarios) {
        const auto name = scenario.at("name").get_string();
        cch::tests::TempWorkspace workspace;
        apply_neutral_env(workspace.path());
        const auto root = workspace.path() / "case";
        fs::create_directories(root);
        const auto options = options_of(scenario);

        if (name == "parent gitignores apply up to the work-tree root and no further") {
            // The capture listed from the fixture's wt/sub directory.
            write_tree(root, scenario.at("inputs").at("files"));
            auto env = cch::durable::local_environment(root.string());
            auto listing = env->list_files("wt/sub", options, {});
            INFO(std::format("scenario: {}", name));
            REQUIRE(listing.has_value());
            check_listing(*listing, scenario.at("expected"));
        } else if (name == "a git file worktree reads info-exclude from the commondir") {
            // The capture's .git file names an absolute gitdir; the test uses
            // its own temp paths for the same layout.
            const auto main_git = root / "main/.git";
            fs::create_directories(main_git / "worktrees/linked");
            fs::create_directories(main_git / "info");
            std::ofstream(main_git / "HEAD") << "ref: refs/heads/main\n";
            std::ofstream(main_git / "info/exclude") << "excluded-by-common.txt\n";
            std::ofstream(main_git / "worktrees/linked/commondir") << "../..\n";
            write_tree(root, scenario.at("inputs").at("files"));
            const auto linked_git = fs::absolute(main_git / "worktrees/linked");
            std::ofstream(root / "linked/.git") << "gitdir: " << linked_git.string() << "\n";
            auto env = cch::durable::local_environment(root.string());
            auto listing = env->list_files("linked", options, {});
            INFO(std::format("scenario: {}", name));
            REQUIRE(listing.has_value());
            check_listing(*listing, scenario.at("expected"));
        } else if (name == "a git symlink to a directory is a repository marker") {
            write_tree(root, scenario.at("inputs").at("files"));
            fs::create_directories(root / "real.git/info");
            std::ofstream(root / "real.git/HEAD") << "ref: refs/heads/main\n";
            std::ofstream(root / "real.git/info/exclude") << "excluded-by-link.txt\n";
            ::symlink("real.git", (root / ".git").c_str());
            auto env = cch::durable::local_environment(root.string());
            auto listing = env->list_files(".", options, {});
            INFO(std::format("scenario: {}", name));
            REQUIRE(listing.has_value());
            check_listing(*listing, scenario.at("expected"));
        } else if (name == "an unreadable gitignore degrades to no rules") {
            write_tree(root, scenario.at("inputs").at("files"));
            ::chmod((root / ".gitignore").c_str(), 0000);
            auto env = cch::durable::local_environment(root.string());
            auto listing = env->list_files(".", options, {});
            ::chmod((root / ".gitignore").c_str(), 0644);
            INFO(std::format("scenario: {}", name));
            REQUIRE(listing.has_value());
            check_listing(*listing, scenario.at("expected"));
        } else if (name == "GIT_DIR and GIT_WORK_TREE do not enable repository rules") {
            write_tree(root, scenario.at("inputs").at("files"));
            // A real repository elsewhere for the environment to point at.
            const auto repo = workspace.path() / "env-repo";
            fs::create_directories(repo / ".git");
            std::ofstream(repo / ".git/HEAD") << "ref: refs/heads/main\n";
            static EnvGuard git_dir("GIT_DIR");
            static EnvGuard git_work_tree("GIT_WORK_TREE");
            git_dir.set((repo / ".git").string());
            git_work_tree.set(repo.string());
            auto env = cch::durable::local_environment(root.string());
            auto listing = env->list_files(".", options, {});
            INFO(std::format("scenario: {}", name));
            REQUIRE(listing.has_value());
            check_listing(*listing, scenario.at("expected"));
        } else if (name.find("config") != std::string::npos || name.find("excludesFile") != std::string::npos ||
                   name.find("xdg default") != std::string::npos) {
            // Every config scenario sets up its own config files and env; the
            // artifact records which source wins and the expected listing.
            write_tree(root, scenario.at("inputs").at("files"));
            const auto make_excludes = [&](const char* file_name, const char* rule) {
                std::ofstream(workspace.path() / file_name) << rule;
                return (workspace.path() / file_name).string();
            };
            static EnvGuard home("HOME");
            static EnvGuard xdg("XDG_CONFIG_HOME");
            static EnvGuard global("GIT_CONFIG_GLOBAL");
            static EnvGuard system("GIT_CONFIG_SYSTEM");
            const auto global_file = make_excludes("cfg-global.txt", "from-global-config.txt\n");
            const auto home_file = make_excludes("cfg-home.txt", "from-home-config.txt\n");
            const auto xdg_file = make_excludes("cfg-xdg.txt", "from-xdg-config.txt\n");
            const auto system_file = make_excludes("cfg-sys.txt", "from-system-config.txt\n");
            const auto write_config = [&](const fs::path& path, const std::string& value) {
                std::ofstream(path) << "[core]\n\texcludesFile = " << value << "\n";
            };
            fs::create_directories(workspace.path() / "cfg-env");
            fs::create_directories(workspace.path() / "cfg-home");
            fs::create_directories(workspace.path() / "cfg-xdg/git");
            fs::create_directories(workspace.path() / "cfg-sys");
            write_config(workspace.path() / "cfg-env/gitconfig", global_file);
            write_config(workspace.path() / "cfg-home/.gitconfig", home_file);
            write_config(workspace.path() / "cfg-xdg/git/config", xdg_file);
            write_config(workspace.path() / "cfg-sys/gitconfig", system_file);
            home.set((workspace.path() / "cfg-home").string());
            xdg.set((workspace.path() / "cfg-xdg").string());
            system.set((workspace.path() / "cfg-sys/gitconfig").string());
            global.set(workspace.path() / "cfg-missing"); // the default; overridden below
            if (name == "GIT_CONFIG_GLOBAL wins over home xdg and system config")
                global.set(workspace.path() / "cfg-env/gitconfig");
            if (name == "a quoted excludesFile with tilde expands against home") {
                std::ofstream(workspace.path() / "cfg-home/my-excludes.txt") << "from-tilde.txt\n";
                write_config(workspace.path() / "cfg-env/gitconfig-quoted", "\"~/my-excludes.txt\"");
                global.set(workspace.path() / "cfg-env/gitconfig-quoted");
                (void)make_excludes;
            }
            if (name == "the xdg default ignore file applies without any config") {
                fs::create_directories(workspace.path() / "cfg-default-xdg/git");
                std::ofstream(workspace.path() / "cfg-default-xdg/git/ignore") << "from-xdg-default.txt\n";
                xdg.set((workspace.path() / "cfg-default-xdg").string());
                home.set((workspace.path() / "cfg-env").string());
                global.set(workspace.path() / "cfg-missing");
                system.set(workspace.path() / "cfg-missing");
            }
            auto env = cch::durable::local_environment(root.string());
            auto listing = env->list_files(".", options, {});
            INFO(std::format("scenario: {}", name));
            REQUIRE(listing.has_value());
            check_listing(*listing, scenario.at("expected"));
            ++index;
            continue;
        } else {
            run_scenario(scenario, root, options);
        }
        ++index;
    }
    CHECK(index == 14);
}

// Regression for issue #1227: a tilde that is not a leading home-directory
// prefix must stay literal, matching frozen pi (packages/durable/src/env/node.ts)
// and the repo's other tilde expansions (WorkspacePathResolution, ShellResolver).
// The excludes file lives under HOME and its name carries a non-leading tilde:
// expanding every tilde corrupts the path into <home>/tilde<home>/excludes.txt
// and the rule never applies.
TEST_CASE("Global excludesFile expands only a leading tilde", "[durable][environment][issue1227]") {
    cch::tests::TempWorkspace workspace;
    apply_neutral_env(workspace.path());
    static EnvGuard home("HOME");
    static EnvGuard xdg("XDG_CONFIG_HOME");
    static EnvGuard global("GIT_CONFIG_GLOBAL");
    static EnvGuard system("GIT_CONFIG_SYSTEM");
    const auto root = workspace.path() / "work";
    fs::create_directories(root / ".git");
    std::ofstream(root / ".git/HEAD") << "ref: refs/heads/main\n";
    std::ofstream(root / "from-tilde.txt");
    std::ofstream(root / "keep.txt");
    fs::create_directories(workspace.path() / "cfg-home");
    std::ofstream(workspace.path() / "cfg-home/tilde~excludes.txt") << "from-tilde.txt\n";
    fs::create_directories(workspace.path() / "cfg-env");
    std::ofstream(workspace.path() / "cfg-env/gitconfig") << "[core]\n\texcludesFile = ~/tilde~excludes.txt\n";
    home.set((workspace.path() / "cfg-home").string());
    xdg.set((workspace.path() / "cfg-no-xdg").string());
    global.set((workspace.path() / "cfg-env/gitconfig").string());
    system.set((workspace.path() / "cfg-no-system").string());
    auto env = cch::durable::local_environment(root.string());
    const auto listing = env->list_files(".", {}, {});
    REQUIRE(listing.has_value());
    std::vector<std::string> paths;
    for (const auto& entry : listing->entries)
        paths.push_back(entry.path);
    CHECK(paths == std::vector<std::string>{"keep.txt"});
}

TEST_CASE("Durable tree listing never loads an unbounded directory into memory", "[durable][environment][issue1158]") {
    cch::tests::TempWorkspace workspace;
    apply_neutral_env(workspace.path());
    const auto root = workspace.path() / "wide";
    fs::create_directories(root);
    constexpr std::size_t kEntries = 20'000;
    for (std::size_t i = 0; i < kEntries; ++i) {
        std::ofstream(root / ("f" + std::to_string(i)), std::ios::binary | std::ios::trunc);
    }
    auto env = cch::durable::local_environment(root.string());
    cch::durable::TreeListingOptions options;
    options.gitignore = false;
    options.max_entries = 100;
    const auto before = [] {
        std::ifstream status("/proc/self/status");
        std::string line;
        while (std::getline(status, line))
            if (line.starts_with("VmHWM:")) return std::stoull(line.substr(line.find_first_of("0123456789"))) * 1024;
        return 0ull;
    }();
    auto listing = env->list_files(".", options, {});
    const auto after = [] {
        std::ifstream status("/proc/self/status");
        std::string line;
        while (std::getline(status, line))
            if (line.starts_with("VmHWM:")) return std::stoull(line.substr(line.find_first_of("0123456789"))) * 1024;
        return 0ull;
    }();
    REQUIRE(listing.has_value());
    CHECK(listing->entries.size() == 100);
    CHECK(listing->truncated);
    // The listing stopped at its bound: names are streamed and the truncated
    // prefix is retained, so the high-water mark stays far below a full
    // materialization of 20k entries.
    CHECK(after - before < 8 * 1024 * 1024);
}

TEST_CASE("Durable tree listing observes cancellation between entries", "[durable][environment][issue1198]") {
    cch::tests::TempWorkspace workspace;
    apply_neutral_env(workspace.path());
    const auto root = workspace.path() / "cancel";
    fs::create_directories(root / ".git");
    std::ofstream(root / ".git/HEAD") << "ref: refs/heads/main\n";
    for (int i = 0; i < 200; ++i) std::ofstream(root / ("f" + std::to_string(i)), std::ios::binary | std::ios::trunc);
    auto env = cch::durable::local_environment(root.string());
    std::stop_source source;
    source.request_stop();
    auto listing = env->list_files(".", {}, source.get_token());
    REQUIRE_FALSE(listing.has_value());
    CHECK(listing.error().code == cch::support::ErrorCode::Cancelled);
}

TEST_CASE(
        "Durable tree listing bounds the ignore-rule memory with a typed error", "[durable][environment][issue1198]") {
    cch::tests::TempWorkspace workspace;
    apply_neutral_env(workspace.path());
    const auto root = workspace.path() / "rules";
    fs::create_directories(root / ".git");
    std::ofstream(root / ".git/HEAD") << "ref: refs/heads/main\n";
    // 50,001 rules exceed the frozen-mandated 50,000 rule budget.
    std::ofstream rules(root / ".gitignore", std::ios::binary | std::ios::trunc);
    for (std::size_t i = 0; i < 50'001; ++i)
        rules << "rule-" << i << "\n";
    auto env = cch::durable::local_environment(root.string());
    auto listing = env->list_files(".", {}, {});
    REQUIRE_FALSE(listing.has_value());
    CHECK(listing.error().code == cch::support::ErrorCode::Validation);
    CHECK(listing.error().message.find("50000") != std::string::npos);

    // And 4 MiB of rule text exceeds the byte budget even with few rules.
    const auto big = workspace.path() / "bytes";
    fs::create_directories(big / ".git");
    std::ofstream(big / ".git/HEAD") << "ref: refs/heads/main\n";
    std::ofstream big_rules(big / ".gitignore", std::ios::binary | std::ios::trunc);
    for (std::size_t i = 0; i < 40; ++i)
        big_rules << std::string(120'000, 'a' + static_cast<char>(i % 26)) << "\n";
    auto big_env = cch::durable::local_environment(big.string());
    auto big_listing = big_env->list_files(".", {}, {});
    REQUIRE_FALSE(big_listing.has_value());
    CHECK(big_listing.error().code == cch::support::ErrorCode::Validation);
    CHECK((big_listing.error().message.find("4 MiB") != std::string::npos ||
            big_listing.error().message.find("4194304") != std::string::npos));
}
