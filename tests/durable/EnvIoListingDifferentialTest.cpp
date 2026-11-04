#include <cch/durable/Environment.hpp>

#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <chrono>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

// The frozen pi-env daemon is the acceptance authority for the tree listing
// (#1200): this harness runs it and the C++ listing over the same fixture
// trees and compares the complete result - payload records in order, the
// entry count, and the truncated flag. It is a test asset: any of the slices
// can extend the fixture matrix, and it documents a skip when the daemon or
// its toolchain is unavailable.

constexpr std::string_view kBaselineRevision = "71023aec1546d6d677a65669c5665c0927ce0dd0";

std::string daemon_path() {
    if (const char* from_env = std::getenv("PI_ENV_DAEMON"); from_env != nullptr && *from_env != '\0') return from_env;
    const fs::path source_dir = CCH_SOURCE_DIR;
    const auto relative = source_dir.parent_path() / "pi/packages/env/daemon/target/debug/pi-env";
    if (fs::exists(relative)) return relative.string();
    return {};
}

// A minimal SHA-256 so the harness records the daemon binary digest without
// shelling out.
class Sha256 {
public:
    Sha256() {
        state_.fill(0);
        state_[0] = 0x6a09e667;
        state_[1] = 0xbb67ae85;
        state_[2] = 0x3c6ef372;
        state_[3] = 0xa54ff53a;
        state_[4] = 0x510e527f;
        state_[5] = 0x9b05688c;
        state_[6] = 0x1f83d9ab;
        state_[7] = 0x5be0cd19;
    }

    void update(std::string_view bytes) {
        total_ += bytes.size();
        while (!bytes.empty()) {
            const std::size_t take = std::min(bytes.size(), buffer_.size() - buffered_);
            std::memcpy(buffer_.data() + buffered_, bytes.data(), take);
            buffered_ += take;
            bytes.remove_prefix(take);
            if (buffered_ == buffer_.size()) {
                transform(buffer_.data());
                buffered_ = 0;
            }
        }
    }

    std::string hex() {
        // Up to two padding blocks: 0x80, zeros to 56 mod 64, then the
        // 8-byte bit count.
        std::array<std::uint8_t, 128> tail{};
        const std::uint64_t bit_count = total_ * 8;
        tail[0] = 0x80;
        std::size_t pad = buffered_ < 56 ? 56 - buffered_ : 120 - buffered_;
        for (std::size_t i = 0; i < 8; ++i)
            tail[pad + i] = static_cast<std::uint8_t>((bit_count >> (56 - i * 8)) & 0xff);
        update(std::string_view(reinterpret_cast<const char*>(tail.data()), pad + 8));
        std::string out;
        for (const std::uint32_t word : state_)
            out += std::format("{:08x}", word);
        return out;
    }

private:
    static std::uint32_t rotr(std::uint32_t x, std::uint32_t n) { return (x >> n) | (x << (32 - n)); }

    void transform(const std::uint8_t* block) {
        static constexpr std::uint32_t k[64] = {0x428a2f98,
                0x71374491,
                0xb5c0fbcf,
                0xe9b5dba5,
                0x3956c25b,
                0x59f111f1,
                0x923f82a4,
                0xab1c5ed5,
                0xd807aa98,
                0x12835b01,
                0x243185be,
                0x550c7dc3,
                0x72be5d74,
                0x80deb1fe,
                0x9bdc06a7,
                0xc19bf174,
                0xe49b69c1,
                0xefbe4786,
                0x0fc19dc6,
                0x240ca1cc,
                0x2de92c6f,
                0x4a7484aa,
                0x5cb0a9dc,
                0x76f988da,
                0x983e5152,
                0xa831c66d,
                0xb00327c8,
                0xbf597fc7,
                0xc6e00bf3,
                0xd5a79147,
                0x06ca6351,
                0x14292967,
                0x27b70a85,
                0x2e1b2138,
                0x4d2c6dfc,
                0x53380d13,
                0x650a7354,
                0x766a0abb,
                0x81c2c92e,
                0x92722c85,
                0xa2bfe8a1,
                0xa81a664b,
                0xc24b8b70,
                0xc76c51a3,
                0xd192e819,
                0xd6990624,
                0xf40e3585,
                0x106aa070,
                0x19a4c116,
                0x1e376c08,
                0x2748774c,
                0x34b0bcb5,
                0x391c0cb3,
                0x4ed8aa4a,
                0x5b9cca4f,
                0x682e6ff3,
                0x748f82ee,
                0x78a5636f,
                0x84c87814,
                0x8cc70208,
                0x90befffa,
                0xa4506ceb,
                0xbef9a3f7,
                0xc67178f2};
        std::uint32_t w[64];
        for (std::size_t i = 0; i < 16; ++i)
            w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(block[i * 4 + 3]);
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t choice = (e & f) ^ (~e & g);
            const std::uint32_t t1 = h + s1 + choice + k[i] + w[i];
            const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_{};
    std::uint64_t total_{};
};

std::string file_sha256(const fs::path& path) {
    Sha256 hash;
    std::ifstream input(path, std::ios::binary);
    std::array<char, 65536> chunk{};
    while (input) {
        input.read(chunk.data(), chunk.size());
        hash.update(std::string_view(chunk.data(), static_cast<std::size_t>(input.gcount())));
    }
    return hash.hex();
}

/// One length-prefixed daemon frame (`docs/protocol.md`): u32 length, u8
/// kind, u32 id, u32 json length, JSON, payload.
struct DaemonClient {
    explicit DaemonClient(std::string binary) {
        int in_pipe[2], out_pipe[2];
        if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0) return;
        const pid_t child = ::fork();
        if (child == 0) {
            ::dup2(in_pipe[0], STDIN_FILENO);
            ::dup2(out_pipe[1], STDOUT_FILENO);
            ::execl(binary.c_str(), binary.c_str(), "serve", "--token", "6363682d646966666572656e7469616c", nullptr);
            _exit(127);
        }
        ::close(in_pipe[0]);
        ::close(out_pipe[1]);
        write_fd_ = in_pipe[1];
        read_fd_ = out_pipe[0];
        // The daemon announces itself with `PI-ENV <token>\n`.
        std::string marker;
        char c = 0;
        while (::read(read_fd_, &c, 1) == 1 && c != '\n')
            marker.push_back(c);
        alive_ = marker.starts_with("PI-ENV ");
    }
    DaemonClient(const DaemonClient&) = delete;
    DaemonClient& operator=(const DaemonClient&) = delete;
    ~DaemonClient() {
        if (write_fd_ >= 0) ::close(write_fd_);
        if (read_fd_ >= 0) ::close(read_fd_);
    }

    struct Reply {
        std::string json{};
        std::vector<std::uint8_t> payload{};
    };

    [[nodiscard]] bool alive() const noexcept { return alive_; }

    std::optional<Reply> request(const std::string& json) {
        if (!alive_) return std::nullopt;
        const std::uint32_t length = 9 + json.size();
        auto put32 = [](std::uint32_t value) {
            return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(value >> 24),
                    static_cast<std::uint8_t>(value >> 16),
                    static_cast<std::uint8_t>(value >> 8),
                    static_cast<std::uint8_t>(value)};
        };
        std::vector<std::uint8_t> frame;
        const auto len = put32(length);
        frame.insert(frame.end(), len.begin(), len.end());
        frame.push_back(1); // REQUEST
        const auto id = put32(next_id_++);
        frame.insert(frame.end(), id.begin(), id.end());
        const auto json_len_bytes = put32(json.size());
        frame.insert(frame.end(), json_len_bytes.begin(), json_len_bytes.end());
        frame.insert(frame.end(), json.begin(), json.end());
        std::size_t written = 0;
        while (written < frame.size()) {
            const ssize_t n = ::write(write_fd_, frame.data() + written, frame.size() - written);
            if (n <= 0) return std::nullopt;
            written += static_cast<std::size_t>(n);
        }
        std::uint8_t header[4]{};
        if (!read_exact(header, 4)) return std::nullopt;
        const std::uint32_t body_len =
                (static_cast<std::uint32_t>(header[0]) << 24) | (static_cast<std::uint32_t>(header[1]) << 16) |
                (static_cast<std::uint32_t>(header[2]) << 8) | static_cast<std::uint32_t>(header[3]);
        std::vector<std::uint8_t> body(body_len);
        if (!read_exact(body.data(), body.size())) return std::nullopt;
        if (body.size() < 9) return std::nullopt;
        const std::uint8_t kind = body[0];
        const std::uint32_t json_len = (static_cast<std::uint32_t>(body[5]) << 24) |
                                       (static_cast<std::uint32_t>(body[6]) << 16) |
                                       (static_cast<std::uint32_t>(body[7]) << 8) | static_cast<std::uint32_t>(body[8]);
        if (kind != 2 || 9 + json_len > body.size()) return std::nullopt; // RESULT expected
        Reply reply;
        reply.json.assign(reinterpret_cast<const char*>(body.data() + 9), json_len);
        reply.payload.assign(body.begin() + 9 + static_cast<std::ptrdiff_t>(json_len), body.end());
        return reply;
    }

private:
    bool read_exact(std::uint8_t* out, std::size_t count) {
        std::size_t done = 0;
        while (done < count) {
            const ssize_t n = ::read(read_fd_, out + done, count - done);
            if (n <= 0) return false;
            done += static_cast<std::size_t>(n);
        }
        return true;
    }

    int write_fd_{-1};
    int read_fd_{-1};
    std::uint32_t next_id_{1};
    bool alive_{};
};

std::string json_escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

/// Runs one fixture tree through both listings and reports the first
/// divergence with the entry index and field, not a pass/fail boolean.
void compare_listing(const fs::path& root,
        const std::string& options_json,
        DaemonClient& daemon,
        const std::shared_ptr<cch::durable::Environment>& env,
        const std::string& scenario) {
    INFO(std::format("differential scenario: {}", scenario));
    const auto reply = daemon.request(
            std::format(R"({{"op":"listFiles","path":"{}"{}}})", json_escape(root.string()), options_json));
    REQUIRE(reply.has_value());
    // Minimal JSON field extraction for the two scalar result fields.
    const auto number_field = [&reply](std::string_view key) -> double {
        const std::string needle = std::string("\"") + std::string(key) + "\":";
        const std::size_t at = reply->json.find(needle);
        if (at == std::string::npos) return -1;
        return std::strtod(reply->json.c_str() + at + needle.size(), nullptr);
    };
    const std::string truncated_field = [&reply] {
        const std::string needle = "\"truncated\":";
        const std::size_t at = reply->json.find(needle);
        return at == std::string::npos ? "" : reply->json.substr(at + needle.size(), 4);
    }();
    const double daemon_count = number_field("count");
    const bool daemon_truncated = truncated_field.starts_with("true");

    cch::durable::TreeListingOptions options;
    options.gitignore = options_json.find("\"gitignore\":false") == std::string::npos;
    options.hidden = options_json.find("\"hidden\":false") == std::string::npos;
    const auto number_option = [&options_json](const char* key) -> std::optional<double> {
        const std::string needle = std::format("\"{}\":", key);
        const std::size_t at = options_json.find(needle);
        if (at == std::string::npos) return std::nullopt;
        return std::strtod(options_json.c_str() + at + needle.size(), nullptr);
    };
    if (const auto entries = number_option("maxEntries")) options.max_entries = static_cast<std::size_t>(*entries);
    if (const auto bytes = number_option("maxBytes")) options.max_bytes = static_cast<std::size_t>(*bytes);
    if (const auto timeout = number_option("timeoutMs"))
        options.timeout = std::chrono::milliseconds(static_cast<long long>(*timeout));
    if (const std::size_t at = options_json.find("\"exclude\":["); at != std::string::npos) {
        const std::size_t end = options_json.find(']', at);
        std::size_t cursor = at;
        while (true) {
            const std::size_t quote = options_json.find('"', cursor);
            if (quote == std::string::npos || quote >= end) break;
            const std::size_t close = options_json.find('"', quote + 1);
            options.exclude.push_back(options_json.substr(quote + 1, close - quote - 1));
            cursor = close + 1;
        }
    }
    auto actual = env->list_files(root.string(), options, {});
    REQUIRE(actual.has_value());
    if (actual->entries.size() != static_cast<std::size_t>(daemon_count)) {
        FAIL_CHECK(std::format("entry count diverges: C++ {} vs daemon {}", actual->entries.size(), daemon_count));
        return;
    }
    if (actual->truncated != daemon_truncated) {
        FAIL_CHECK(std::format("truncated flag diverges: C++ {} vs daemon {}", actual->truncated, daemon_truncated));
        return;
    }
    std::size_t offset = 0;
    for (std::size_t i = 0; i < actual->entries.size(); ++i) {
        const auto& entry = actual->entries[i];
        if (offset >= reply->payload.size()) {
            FAIL_CHECK(std::format("payload ends before entry {} ({})", i, entry.path));
            return;
        }
        const std::uint8_t kind = reply->payload[offset];
        const std::size_t end = std::find(reply->payload.begin() + static_cast<std::ptrdiff_t>(offset) + 1,
                                        reply->payload.end(),
                                        std::uint8_t{0}) -
                                reply->payload.begin();
        if (end == reply->payload.size()) {
            FAIL_CHECK(std::format("payload record {} is not NUL-terminated", i));
            return;
        }
        const std::string daemon_path(
                reinterpret_cast<const char*>(reply->payload.data() + offset + 1), end - offset - 1);
        if (kind != static_cast<std::uint8_t>(entry.kind)) {
            FAIL_CHECK(std::format(
                    "entry {} kind diverges: C++ '{}' vs daemon '{}'", i, entry.kind, static_cast<char>(kind)));
            return;
        }
        if (daemon_path != entry.path) {
            FAIL_CHECK(std::format("entry {} path diverges: C++ '{}' vs daemon '{}'", i, entry.path, daemon_path));
            return;
        }
        offset = end + 1;
    }
    if (offset != reply->payload.size()) {
        FAIL_CHECK(std::format("payload has {} trailing bytes after {} entries",
                reply->payload.size() - offset,
                actual->entries.size()));
    }
}

void write_file(const fs::path& path, const std::string& content = {}) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
}

} // namespace

TEST_CASE("Durable tree listing is byte-for-byte identical to the frozen listFiles daemon",
        "[durable][environment][issue1200]") {
    const std::string binary = daemon_path();
    if (binary.empty() || !fs::exists(binary)) {
        SKIP("the frozen pi-env daemon is unavailable; build it from the baseline checkout with "
             "`cargo build --locked --offline` in packages/env/daemon");
    }
    const std::string digest = file_sha256(binary);
    INFO(std::format("frozen daemon: {} sha256 {} baseline {}", binary, digest, kBaselineRevision));

    cch::tests::TempWorkspace workspace;
    auto env = cch::durable::local_environment(workspace.path().string());
    DaemonClient daemon(binary);
    REQUIRE(daemon.alive());

    // A wide directory, deep nesting, byte-order siblings, hidden names, an
    // ignored subtree with a negation below it, symlinks, a FIFO, an empty
    // directory, and a non-UTF-8 name in one work tree.
    {
        const auto root = workspace.path() / "main";
        write_file(root / ".git/HEAD", "ref: refs/heads/main\n");
        write_file(root / ".gitignore", "build/\n!build/keep.txt\n*.log\n!.hidden-keep\n");
        for (int i = 0; i < 1500; ++i)
            write_file(root / ("w" + std::to_string(i)));
        write_file(root / "deep/a/b/c/leaf.txt");
        write_file(root / "build/drop.txt");
        write_file(root / "build/keep.txt");
        write_file(root / "app.log");
        write_file(root / "keep.txt");
        write_file(root / ".hidden-keep");
        write_file(root / ".hidden/x");
        write_file(root / "empty/");
        ::symlink("keep.txt", (root / "link").c_str());
        ::symlink("missing", (root / "dangling").c_str());
        ::mkfifo((root / "pipe").c_str(), 0600);
        write_file(root / (std::string("bad") + char(0xff) + ".txt"));
        compare_listing(root, ",\"gitignore\":true", daemon, env, "work tree rules and kinds");
        compare_listing(root, ",\"gitignore\":true,\"hidden\":false", daemon, env, "hidden false");
        compare_listing(root, ",\"gitignore\":false", daemon, env, "gitignore false");
        compare_listing(root, ",\"gitignore\":false,\"hidden\":false", daemon, env, "gitignore and hidden false");
    }

    // Entry and byte bounds: zero, exact, and over, in both directions.
    {
        const auto root = workspace.path() / "bounds";
        write_file(root / ".git/HEAD");
        write_file(root / "a");
        write_file(root / "b");
        write_file(root / "c");
        compare_listing(root, ",\"gitignore\":false,\"maxEntries\":0", daemon, env, "maxEntries zero");
        compare_listing(root, ",\"gitignore\":false,\"maxEntries\":2", daemon, env, "maxEntries under");
        compare_listing(root, ",\"gitignore\":false,\"maxEntries\":3", daemon, env, "maxEntries exact");
        compare_listing(root, ",\"gitignore\":false,\"maxEntries\":9", daemon, env, "maxEntries over");
        compare_listing(root, ",\"gitignore\":false,\"maxBytes\":0", daemon, env, "maxBytes zero");
        compare_listing(root, ",\"gitignore\":false,\"maxBytes\":9", daemon, env, "maxBytes exact");
        compare_listing(root, ",\"gitignore\":false,\"maxBytes\":10", daemon, env, "maxBytes over");
    }

    // `.git` as a file (linked worktree through gitdir + commondir) and a
    // nested repository blocking outer rules.
    {
        const auto root = workspace.path() / "layouts";
        const auto main_git = root / "main/.git";
        write_file(main_git / "HEAD");
        write_file(main_git / "info/exclude", "excluded-by-common.txt\n");
        write_file(main_git / "worktrees/linked/commondir", "../..\n");
        write_file(root / "linked/.gitignore", "dropped.txt\n");
        write_file(root / "linked/dropped.txt");
        write_file(root / "linked/excluded-by-common.txt");
        write_file(root / "linked/keep.txt");
        std::ofstream(root / "linked/.git")
                << "gitdir: " << fs::absolute(main_git / "worktrees/linked").string() << "\n";
        compare_listing(root / "linked", ",\"gitignore\":true", daemon, env, "git file worktree");

        write_file(root / "outer/.git/HEAD");
        write_file(root / "outer/.gitignore", "root-only.txt\nshared.txt\n");
        write_file(root / "outer/root-only.txt");
        write_file(root / "outer/shared.txt");
        write_file(root / "outer/inner/.git/HEAD");
        write_file(root / "outer/inner/.gitignore", "!shared.txt\ninner-only.txt\n");
        write_file(root / "outer/inner/shared.txt");
        write_file(root / "outer/inner/inner-only.txt");
        write_file(root / "outer/inner/root-only.txt");
        write_file(root / "outer/inner/keep.txt");
        compare_listing(root / "outer", ",\"gitignore\":true", daemon, env, "nested repository");
    }

    // Name exclusions.
    {
        const auto root = workspace.path() / "exclude";
        write_file(root / ".git/HEAD");
        write_file(root / "node_modules/m/i.js");
        write_file(root / "out/x.js");
        write_file(root / "keep.txt");
        compare_listing(root, ",\"gitignore\":false,\"exclude\":[\"node_modules\"]", daemon, env, "excluded name");
    }
}
