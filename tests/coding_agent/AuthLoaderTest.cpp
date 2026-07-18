#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../support/TempWorkspace.hpp"

#include "cch/coding_agent/AuthLoader.hpp"

#include <filesystem>
#include <fstream>
#include <string>

TEST_CASE("AuthLoader loads api_key entries from auth.json", "[coding_agent][auth]") {
    cch::tests::TempWorkspace workspace;
    auto path = workspace.path() / "auth.json";
    {
        std::ofstream file(path);
        file << R"({
            "kimi-coding": {"type": "api_key", "key": "sk-kimi-test"},
            "deepseek": {"type": "api_key", "key": "sk-ds-test"},
            "empty": {"type": "api_key", "key": ""}
        })";
    }

    auto loaded = cch::coding_agent::AuthLoader::load(path);
    REQUIRE(loaded);
    CHECK(loaded->size() == 2);
    REQUIRE(loaded->find("kimi-coding") != loaded->end());
    CHECK(loaded->at("kimi-coding").type == "api_key");
    CHECK(loaded->at("kimi-coding").key == "sk-kimi-test");
    REQUIRE(loaded->find("deepseek") != loaded->end());
    CHECK(loaded->at("deepseek").key == "sk-ds-test");
    CHECK(loaded->find("empty") == loaded->end());
}

TEST_CASE("AuthLoader returns empty map for missing file", "[coding_agent][auth]") {
    cch::tests::TempWorkspace workspace;
    auto loaded = cch::coding_agent::AuthLoader::load(workspace.path() / "missing.json");
    REQUIRE(loaded);
    CHECK(loaded->empty());
}

TEST_CASE("AuthLoader reports malformed JSON", "[coding_agent][auth]") {
    cch::tests::TempWorkspace workspace;
    auto path = workspace.path() / "bad.json";
    {
        std::ofstream file(path);
        file << "not json";
    }

    auto loaded = cch::coding_agent::AuthLoader::load(path);
    REQUIRE_FALSE(loaded);
}
