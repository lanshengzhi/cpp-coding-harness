#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace cch::tests {

class TempWorkspace {
public:
    TempWorkspace() {
        auto base = std::filesystem::temp_directory_path();
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = base / ("cpp-harness-test-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    TempWorkspace(const TempWorkspace&) = delete;
    TempWorkspace& operator=(const TempWorkspace&) = delete;

    ~TempWorkspace() { std::error_code ec; std::filesystem::remove_all(path_, ec); }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    void write(std::string relative, std::string content) const {
        auto target = path_ / relative;
        std::filesystem::create_directories(target.parent_path());
        std::ofstream output(target, std::ios::binary | std::ios::trunc);
        output << content;
    }

    [[nodiscard]] std::string read(std::string relative) const {
        std::ifstream input(path_ / relative, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

private:
    std::filesystem::path path_;
};

} // namespace cch::tests
