#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../include/cch/coding_agent/ProjectTrust.hpp"
#include "../support/TempWorkspace.hpp"

#include <filesystem>
#include <fstream>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

using namespace cch;

TEST_CASE("ProjectTrustStore round-trips decisions and ancestor lookup", "[coding_agent][project-trust]") {
    tests::TempWorkspace workspace;
    auto trust_path = workspace.path() / "trust.json";
    coding_agent::ProjectTrustStore store{trust_path};

    auto parent = workspace.path() / "repo";
    auto child = parent / "subdir";
    std::filesystem::create_directories(child);

    auto written = store.setMany({{.path = parent.string(), .decision = coding_agent::ProjectTrustDecision::Trusted}});
    REQUIRE(written.has_value());

    auto entry = store.getEntry(child);
    REQUIRE(entry.has_value());
    REQUIRE(entry->has_value());
    CHECK((*entry)->decision == coding_agent::ProjectTrustDecision::Trusted);
    CHECK((*entry)->path == std::filesystem::weakly_canonical(parent).string());
}

TEST_CASE("ProjectTrustStore child override and removal exposes parent", "[coding_agent][project-trust]") {
    tests::TempWorkspace workspace;
    auto trust_path = workspace.path() / "trust.json";
    coding_agent::ProjectTrustStore store{trust_path};

    auto parent = workspace.path() / "repo";
    auto child = parent / "child";
    std::filesystem::create_directories(child);

    REQUIRE(store.setMany({
        {.path = parent.string(), .decision = coding_agent::ProjectTrustDecision::Trusted},
        {.path = child.string(), .decision = coding_agent::ProjectTrustDecision::Untrusted},
    }).has_value());

    auto child_entry = store.getEntry(child);
    REQUIRE(child_entry.has_value());
    REQUIRE(child_entry->has_value());
    CHECK((*child_entry)->decision == coding_agent::ProjectTrustDecision::Untrusted);

    REQUIRE(store.setMany({{.path = child.string(), .decision = coding_agent::ProjectTrustDecision::Unknown}}).has_value());
    child_entry = store.getEntry(child);
    REQUIRE(child_entry.has_value());
    REQUIRE(child_entry->has_value());
    CHECK((*child_entry)->decision == coding_agent::ProjectTrustDecision::Trusted);
}

TEST_CASE("ProjectTrustStore malformed values fail closed", "[coding_agent][project-trust]") {
    tests::TempWorkspace workspace;
    auto trust_path = workspace.path() / "trust.json";
    std::ofstream(trust_path) << R"({"/tmp/project":"yes"})";
    coding_agent::ProjectTrustStore store{trust_path};

    auto entry = store.getEntry(workspace.path());
    CHECK_FALSE(entry.has_value());
}

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE("ProjectTrustStore rejects symlinked trust store", "[coding_agent][project-trust]") {
    tests::TempWorkspace workspace;
    auto real_path = workspace.path() / "real-trust.json";
    auto link_path = workspace.path() / "trust.json";
    std::ofstream(real_path) << R"({})";
    std::filesystem::create_symlink(real_path, link_path);
    coding_agent::ProjectTrustStore store{link_path};

    auto entry = store.getEntry(workspace.path());
    CHECK_FALSE(entry.has_value());
}
#endif

TEST_CASE("resolve_project_trust follows override no-resource store default order", "[coding_agent][project-trust]") {
    tests::TempWorkspace workspace;
    auto trust_path = workspace.path() / "trust.json";
    coding_agent::ProjectTrustStore store{trust_path};

    auto resolved = coding_agent::resolve_project_trust(
        workspace.path(), true, store, coding_agent::DefaultProjectTrust::Ask, true);
    CHECK(resolved.decision == coding_agent::ProjectTrustDecision::Trusted);
    CHECK(resolved.source == coding_agent::ProjectTrustSource::CliOverride);

    resolved = coding_agent::resolve_project_trust(
        workspace.path(), true, store, coding_agent::DefaultProjectTrust::Ask, false);
    CHECK(resolved.decision == coding_agent::ProjectTrustDecision::Untrusted);
    CHECK(resolved.source == coding_agent::ProjectTrustSource::CliOverride);

    resolved = coding_agent::resolve_project_trust(
        workspace.path(), false, store, coding_agent::DefaultProjectTrust::Ask, std::nullopt);
    CHECK(resolved.decision == coding_agent::ProjectTrustDecision::Trusted);
    CHECK(resolved.source == coding_agent::ProjectTrustSource::NoProjectResources);

    REQUIRE(store.setMany({{.path = workspace.path().string(), .decision = coding_agent::ProjectTrustDecision::Trusted}}).has_value());
    resolved = coding_agent::resolve_project_trust(
        workspace.path(), true, store, coding_agent::DefaultProjectTrust::Never, std::nullopt);
    CHECK(resolved.decision == coding_agent::ProjectTrustDecision::Trusted);
    CHECK(resolved.source == coding_agent::ProjectTrustSource::StoreEntry);

    tests::TempWorkspace no_store_workspace;
    coding_agent::ProjectTrustStore missing_store{no_store_workspace.path() / "missing" / "trust.json"};
    resolved = coding_agent::resolve_project_trust(
        no_store_workspace.path(), true, missing_store, coding_agent::DefaultProjectTrust::Always, std::nullopt);
    CHECK(resolved.decision == coding_agent::ProjectTrustDecision::Trusted);
    CHECK(resolved.source == coding_agent::ProjectTrustSource::DefaultAlways);

    resolved = coding_agent::resolve_project_trust(
        no_store_workspace.path(), true, missing_store, coding_agent::DefaultProjectTrust::Never, std::nullopt);
    CHECK(resolved.decision == coding_agent::ProjectTrustDecision::Untrusted);
    CHECK(resolved.source == coding_agent::ProjectTrustSource::DefaultNever);

    resolved = coding_agent::resolve_project_trust(
        no_store_workspace.path(), true, missing_store, coding_agent::DefaultProjectTrust::Ask, std::nullopt);
    CHECK(resolved.decision == coding_agent::ProjectTrustDecision::Untrusted);
    CHECK(resolved.source == coding_agent::ProjectTrustSource::DefaultAskNoUi);
}

TEST_CASE("resolve_project_trust fails closed on malformed store unless overridden", "[coding_agent][project-trust]") {
    tests::TempWorkspace workspace;
    auto trust_path = workspace.path() / "trust.json";
    std::ofstream(trust_path) << "{not json";
    coding_agent::ProjectTrustStore store{trust_path};

    auto resolved = coding_agent::resolve_project_trust(
        workspace.path(), true, store, coding_agent::DefaultProjectTrust::Always, std::nullopt);
    CHECK(resolved.decision == coding_agent::ProjectTrustDecision::Untrusted);
    CHECK(resolved.source == coding_agent::ProjectTrustSource::StoreUnavailable);
    CHECK_FALSE(resolved.diagnostics.empty());

    resolved = coding_agent::resolve_project_trust(
        workspace.path(), true, store, coding_agent::DefaultProjectTrust::Ask, true);
    CHECK(resolved.decision == coding_agent::ProjectTrustDecision::Trusted);
    CHECK(resolved.source == coding_agent::ProjectTrustSource::CliOverride);

    resolved = coding_agent::resolve_project_trust(
        workspace.path(), true, store, coding_agent::DefaultProjectTrust::Always, false);
    CHECK(resolved.decision == coding_agent::ProjectTrustDecision::Untrusted);
    CHECK(resolved.source == coding_agent::ProjectTrustSource::CliOverride);
}
